#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/cache.h"
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "threads/synch.h"

#define SID_INDEX 5
#define DID_INDEX 7
#define MAX_INDEX 8
#define DIRECT_LIMIT (SID_INDEX)
#define NUM_PER_SECTOR (BLOCK_SECTOR_SIZE / sizeof (unsigned))
#define SID_LIMIT (DIRECT_LIMIT + (DID_INDEX - SID_INDEX) * NUM_PER_SECTOR)
#define DID_LIMIT (SID_LIMIT + (MAX_INDEX - DID_INDEX) * \
                   NUM_PER_SECTOR * NUM_PER_SECTOR)




/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
  {
    bool present;
    off_t length;                       /* File size in bytes. */
    unsigned arr[8];
  };

static struct lock inumber_lock;
      

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, BLOCK_SECTOR_SIZE);
}

/* In-memory inode. */
struct inode 
  {
    struct list_elem elem;              /* Element in inode list. */
    unsigned inumber;              /* Sector number of disk location. */
    int open_cnt;                       /* Number of openers. */
    bool removed;                       /* True if deleted, false otherwise. */
    int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
    struct inode_disk data;             /* Inode content. */
  };


static bool
sector_fixup (unsigned *sector)
{
  if (*sector == 0)
    {
      if (!free_map_allocate (1, sector))
        PANIC ("Disk full!");
      cache_sector_add (*sector);
      return true;
    }

  return false;
}  

static void
sector_fixup_disk (unsigned from_sector, unsigned *to_sector, 
                          unsigned index)
{
  cache_sector_read (from_sector, to_sector, sizeof (unsigned), 
                     index * sizeof (unsigned));
  if (sector_fixup (to_sector))
    cache_sector_write (from_sector, to_sector, sizeof (unsigned), 
                        index * sizeof (unsigned));
}

static bool
inode_write_to_table (unsigned inumber, struct inode_disk *disk_inode)
{ 
  unsigned inodes_per_page = BLOCK_SECTOR_SIZE / sizeof *disk_inode;
  block_sector_t table_sector = inumber / inodes_per_page;
  unsigned table_ofs = (inumber % inodes_per_page) * sizeof *disk_inode;
  if (table_sector >= INODE_TABLE_SECTORS)
    return false;
  cache_sector_write (table_sector, disk_inode, sizeof *disk_inode, 
                      table_ofs);
  return true;
}

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t
byte_to_sector (const struct inode *inode, off_t pos)
{
  if (inode->inumber == 1)
  	hex_dump (0, &inode->data.arr, 8, false);
  // CHECK THI SLOGIC, reanem index ofsets etc
  ASSERT (inode != NULL);
  unsigned sector;
  unsigned index = pos / BLOCK_SECTOR_SIZE;
  if (index < DIRECT_LIMIT)
    {
      sector_fixup (&inode->data.arr[index]);
      sector = inode->data.arr[index];
    }
  else if (index < SID_LIMIT)
    {
      unsigned sid_index = SID_INDEX + (index - DIRECT_LIMIT) / 
                           NUM_PER_SECTOR;
      unsigned d_index = ((index - DIRECT_LIMIT) % NUM_PER_SECTOR);
      sector_fixup (&inode->data.arr[sid_index]);
      sector_fixup_disk (inode->data.arr[sid_index], &sector, d_index);  
    }
  else if (index < DID_LIMIT)
    {
      unsigned d_sector;
      unsigned did_index = DID_INDEX + (index - SID_LIMIT) /
                           (NUM_PER_SECTOR * NUM_PER_SECTOR);
      unsigned sid_index = ((index - SID_LIMIT) / NUM_PER_SECTOR) %
                           NUM_PER_SECTOR;
      unsigned d_index = ((index - SID_LIMIT) % NUM_PER_SECTOR);
      sector_fixup (&inode->data.arr[did_index]);
      sector_fixup_disk (inode->data.arr[did_index], &d_sector, sid_index);  
      sector_fixup_disk (d_sector, &sector, d_index);  
    }
/* error check ? need to be every time? */
    inode_write_to_table (inode->inumber, &inode->data);   
    return sector;
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void
inode_init (void) 
{
  list_init (&open_inodes);
  lock_init (&inumber_lock);
}

bool
inode_assign_inumber (unsigned *inumber)
{
  struct inode_disk disk_inode;
  unsigned inodes_per_page = BLOCK_SECTOR_SIZE / sizeof disk_inode;
  for (unsigned i = 0; i < INODE_TABLE_SECTORS; i++)
    {
      for (unsigned j = 0; i < inodes_per_page; j++)
        {
          lock_acquire (&inumber_lock);
          cache_sector_read (i, &disk_inode, sizeof disk_inode, 
                             j * sizeof disk_inode);
          if (!disk_inode.present)
            {
              disk_inode.present = true;
              cache_sector_write (i, &disk_inode, sizeof disk_inode, 
                                  j * sizeof disk_inode);
              lock_release (&inumber_lock);
              *inumber = i * inodes_per_page + j;
              return true;
            }
          lock_release (&inumber_lock);
        }
    }

  PANIC ("Reached maximum number of files!");
  return false;
}

void
inode_release_inumber (unsigned inumber)
{
  struct inode_disk disk_inode;
  unsigned inodes_per_page = BLOCK_SECTOR_SIZE / sizeof (disk_inode);
  block_sector_t sector = inumber / inodes_per_page;
  unsigned ofs = sizeof (disk_inode) * (inumber % inodes_per_page);
  memset (&disk_inode, 0, sizeof (disk_inode));
  cache_sector_write (sector, &disk_inode, sizeof (disk_inode), ofs);
}




/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create (unsigned inumber, off_t length)
{
  // Should I preallocate?
  ASSERT (length >= 0);

  struct inode_disk disk_inode; 
  
  disk_inode.length = length;
  disk_inode.present = true;
  memset (&disk_inode.arr, 0, sizeof (disk_inode.arr));
  return inode_write_to_table (inumber, &disk_inode);
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (unsigned inumber)
{
  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e)) 
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->inumber == inumber) 
        {
          inode_reopen (inode);
          return inode; 
        }
    }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
    return NULL;

  /* Initialize. */
  list_push_front (&open_inodes, &inode->elem);
  inode->inumber = inumber;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  unsigned inodes_per_sector = BLOCK_SECTOR_SIZE / sizeof inode->data;
  block_sector_t sector = inumber / inodes_per_sector;
  unsigned ofs = (inumber % inodes_per_sector) * sizeof inode->data;
  cache_sector_read (sector, &inode->data, sizeof inode->data, ofs);

  return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t
inode_get_inumber (const struct inode *inode)
{
  return inode->inumber;
}

bool
inode_free_map_create (unsigned inumber, unsigned length, unsigned *sector)
{
  unsigned num_sectors = bytes_to_sectors (length);
  ASSERT (num_sectors >= 1);
  if (!inode_create (inumber, length))
    return false;

  struct inode *inode = inode_open (inumber);
  
  unsigned i = 0;
  unsigned _sector = INODE_TABLE_SECTORS;
  while (i < SID_INDEX)
    {
      inode->data.arr[i++] = _sector++;
      if (--num_sectors == 0)
        {
          inode_write_to_table (inumber, &inode->data);
          inode_close (inode);
          *sector = _sector - INODE_TABLE_SECTORS;
          return true;
        }
    }
  while (i < DID_INDEX)
    {
      inode->data.arr[i] = _sector++;
      for (unsigned j = 0; j < NUM_PER_SECTOR; j++)
        {
          cache_sector_write (inode->data.arr[i], &_sector, sizeof (unsigned),
                              j * sizeof (unsigned));
          _sector++;
          if (--num_sectors == 0)
            {
              inode_write_to_table (inumber, &inode->data);
              inode_close (inode);
              *sector = _sector - INODE_TABLE_SECTORS;
              return true;
            }
        }
      i++;
    }
  while (i < MAX_INDEX)
    {
      inode->data.arr[i] = _sector++;
      for (unsigned j = 0; j < NUM_PER_SECTOR; j++)
        {
          cache_sector_write (inode->data.arr[i], &_sector, sizeof (unsigned),
                              j * sizeof (unsigned));
          unsigned saved_sector = _sector++;
          for (unsigned k = 0; k < NUM_PER_SECTOR; k++)
            {
              cache_sector_write (saved_sector, &_sector, sizeof (unsigned),
                                  k * sizeof (unsigned));
              _sector++;
              if (--num_sectors == 0)
                {
                  inode_write_to_table (inumber, &inode->data);
                  inode_close (inode);
                  *sector = _sector - INODE_TABLE_SECTORS;
                  return true;
                }
            }
        }
    }

  inode_close (inode);
  return false;
}
          
  


static void
sector_deallocate (unsigned sector)
{
 // Mark as closed in cache?
  if (sector != 0)
    free_map_release (sector, 1);
}

static void
sector_deallocate_disk (unsigned from_sector, unsigned depth)
{
  if (from_sector == 0)
    return;
  depth--;
  unsigned to_sector;
  for (unsigned j = 0; j < BLOCK_SECTOR_SIZE / sizeof (block_sector_t); j++)
    {
      cache_sector_read (from_sector, &to_sector, sizeof (unsigned),
                         j * sizeof (unsigned));
      if (depth > 0)
        sector_deallocate_disk (to_sector, depth);
      sector_deallocate (to_sector);
    }
  sector_deallocate (from_sector);
}

static void
inode_release_sectors (struct inode *inode)
{
  for (unsigned i = 0; i < SID_INDEX; i++)
    sector_deallocate (inode->data.arr[i]);
  for (unsigned i = SID_INDEX; i < DID_INDEX; i++)
    sector_deallocate_disk (inode->data.arr[i], 1);
  for (unsigned i = DID_INDEX; i < MAX_INDEX; i++)
    sector_deallocate_disk (inode->data.arr[i], 2);
}

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode) 
{
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0)
    {
      /* Remove from inode list and release lock. */
      list_remove (&inode->elem);
 
      /* Deallocate blocks if removed. */
      if (inode->removed) 
        {
          printf ("making sure\n");
          inode_release_inumber (inode->inumber);
          inode_release_sectors (inode); 
        }

      free (inode); 
    }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode) 
{
  ASSERT (inode != NULL);
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) 
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;

  while (size > 0) 
    {
      /* Offset within sector to read from . */
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      /* Disk sector to read from. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      printf ("READ FROM: %u\n", sector_idx);
      cache_sector_read (sector_idx, buffer + bytes_read, 
                         chunk_size, sector_ofs);


      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }

  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset) 
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;
  // NEEDS SYNC FOR SIMULTANEOUS READ/WRITE, DO LATER when getting rid ofglobal lock:70

  if (inode->deny_write_cnt)
    return 0;

  while (size > 0) 
    {
      /* Offset within sector to write to. */
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in sector. */
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < sector_left ? size : sector_left;

      /* Disk sector to write to. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      
      printf ("WRITE TO: %u\n", sector_idx);
      cache_sector_write (sector_idx, buffer + bytes_written,
                          chunk_size, sector_ofs);
    
      // Update file size after so read deosnt get confused
      // What if two writes happen at once?
      inode->data.length = inode->data.length > offset + chunk_size ?
                           inode->data.length : offset + chunk_size;

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }

  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode) 
{
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) 
{
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode)
{
  return inode->data.length;
}
