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

/* Derefences SECTOR. If it is 0, this function allocates
   a sector a places its number at SECTOR. Returns false
   if a sector allocation fails. */
static bool
sector_fixup (unsigned *sector)
{
  if (*sector == 0)
    {
      if (!free_map_allocate (1, sector))
        return false;
      cache_sector_add (*sector);
    }

  return true;
}  

/* Gets a sector number from an index sector FROM_SECTOR,
   at index INDEX. Allocates and writes back a sector number 
   if that index of FROM_SECTOR had not previously been allocated.
   Return false if a sector allocation fails. */
static bool
sector_fixup_disk (unsigned from_sector, unsigned *to_sector, 
                          unsigned index)
{
  cache_sector_read (from_sector, to_sector, sizeof (unsigned), 
                     index * sizeof (unsigned));
  unsigned prev_sector = *to_sector;
  if (!sector_fixup (to_sector))
    return false;
  if (prev_sector == 0)
    cache_sector_write (from_sector, to_sector, sizeof (unsigned), 
                        index * sizeof (unsigned));

  return true;
}

/* Writes DISK_INODE to the inode table at index INUMBER. */
static void
inode_write_to_table (unsigned inumber, struct inode_disk *disk_inode)
{ 
  unsigned inodes_per_page = BLOCK_SECTOR_SIZE / sizeof *disk_inode;
  block_sector_t table_sector = inumber / inodes_per_page;
  unsigned table_ofs = (inumber % inodes_per_page) * sizeof *disk_inode;
  ASSERT (table_sector < INODE_TABLE_SECTORS);
  cache_sector_write (table_sector, disk_inode, sizeof *disk_inode, 
                      table_ofs);
}

/* Allocates a sector of index INDEX if not already allocated. 
   Allocates intermediate sectors as needed based on level of 
   indirection (DEPTH), if they have not already been allocated.
   Returns the final sector number, and returns -1 if a necessary
   sector allocation fails. */
static unsigned
sector_fixup_depth (struct inode *inode, unsigned start_index, 
                    unsigned prev_limit, unsigned index, unsigned depth)
{
  index -= prev_limit;
  unsigned arr_index  = index;
  for (unsigned i = 0; i < depth; i++)
    arr_index /= NUM_PER_SECTOR;
  
  arr_index += start_index;
  if (!sector_fixup (&inode->data.arr[arr_index]))
    return -1;
  unsigned sector_to = inode->data.arr[arr_index];
  for (unsigned i = 0; i < depth; i++)
    {
      unsigned sector_index = index;
      for (unsigned j = i + 1; j < depth; j++)
        sector_index /= NUM_PER_SECTOR;
      sector_index %= NUM_PER_SECTOR;
      unsigned sector_from = sector_to;
      if (!sector_fixup_disk (sector_from, &sector_to, sector_index))
        return -1;
    }

  inode_write_to_table (inode->inumber, &inode->data);
  return sector_to;
}
      
// NEED SYNCH?
// HOW YO FREE UNUSED PAGES ON RETURN?

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if POS excdeeds the maximum file size, or if a sector
   allocation was needed but no sectors were free. */
static block_sector_t
byte_to_sector (const struct inode *inode, off_t pos)
{
  ASSERT (inode != NULL);
  unsigned sector;
  unsigned index = pos / BLOCK_SECTOR_SIZE;
  if (index < DIRECT_LIMIT)
    return sector_fixup_depth (inode, 0, 0, index, 0);
  else if (index < SID_LIMIT)
    return sector_fixup_depth (inode, SID_INDEX, DIRECT_LIMIT, index, 1);
  else if (index < DID_LIMIT)
    return sector_fixup_depth (inode, DID_INDEX, SID_LIMIT, index, 2);  
 
  return -1;
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

/* Allocates the first available inumber from the inode table,
   and places it at INUMBER. Returns false if no inumbers are
   available (maximum number of files reached). */
bool
inode_assign_inumber (unsigned *inumber)
{
  struct inode_disk disk_inode;
  unsigned inodes_per_page = BLOCK_SECTOR_SIZE / sizeof disk_inode;
  for (unsigned i = 0; i < INODE_TABLE_SECTORS; i++)
    {
      for (unsigned j = 0; j < inodes_per_page; j++)
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
  inode_write_to_table (inumber, &disk_inode);
  return true; //THIS CANT FAIL
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



static struct create_seq_state
  {
    struct inode *inode;
    unsigned cur_sector;
    unsigned file_sectors;
    unsigned *total_sectors;
    unsigned start_sector;
  };


static bool
create_seq_advance (struct create_seq_state *s)
{
  if (--s->file_sectors == 0)
    {
      inode_write_to_table (s->inode->inumber, &s->inode->data);
      inode_close (s->inode);
      *s->total_sectors = s->cur_sector - s->start_sector;
      return true;
    }
  s->cur_sector++;
  return false;
}

static bool
create_seq_advance_sector (unsigned *saved_sector, struct create_seq_state *s,
                           unsigned depth)
{
  *saved_sector = s->cur_sector++;
  if (depth == 0)
    return create_seq_advance (s);
  for (unsigned i = 0; i < NUM_PER_SECTOR; i++)
    {
      cache_sector_write (saved_sector, &s->cur_sector, sizeof (unsigned), 
                          i * sizeof (unsigned));
      unsigned saved_sector_new = s->cur_sector;
      if (create_seq_advance_sector (&saved_sector_new, s, depth))
        return true;
    }
  return false;
}
          

/* Creates an inode for a file of length LENGTH, sequentially assigning 
   sectors starting at START_SECTOR. Returns the populates SECTOR with 
   the number of sectors used, including metadata sectors. The caller is
   responsible for allocating the sectors. Returns false if LENGTH exceeds
   maximum file length. */

bool
inode_create_seq (unsigned inumber, unsigned *sector, unsigned length, 
                  unsigned start_sector)
{
  inode_create (inumber, length);

  struct create_seq_state s;
  s.file_sectors = bytes_to_sectors (length);
  s.inode = inode_open (inumber);
  s.start_sector = start_sector;
  s.cur_sector = start_sector;
  s.total_sectors = sector;

  unsigned i = 0;
  for (; i < SID_INDEX; i++)
    if (create_seq_advance_sector (&s.inode->data.arr[i], &s, 0))
      return true;
  for (; i < DID_INDEX; i++)
    if (create_seq_advance_sector (&s.inode->data.arr[i], &s, 1))
      return true;
  for (; i < MAX_INDEX; i++)
    if (create_seq_advance_sector (&s.inode->data.arr[i], &s, 2))
      return true;

  inode_close (s.inode);
  return false;
}
// TEST THIS FOR LARGE FREE MAPS 
  


static void
sector_deallocate (unsigned sector)
{
  if (sector != 0)
    {
      cache_sector_remove (sector);
      free_map_release (sector, 1);
    }
}

static void
sector_deallocate_disk (unsigned from_sector, unsigned depth)
{
  /* Skip unassigned entries. */
  if (from_sector == 0)
    return;

  if (depth-- == 0)
    {
      sector_deallocate (from_sector);
      return;
    }

  unsigned to_sector;
  for (unsigned j = 0; j < BLOCK_SECTOR_SIZE / sizeof (block_sector_t); j++)
    {
      cache_sector_read (from_sector, &to_sector, sizeof (unsigned),
                         j * sizeof (unsigned));
      sector_deallocate_disk (to_sector, depth);
    }
  sector_deallocate (from_sector);
}

static void
inode_release_sectors (struct inode *inode)
{
  for (unsigned i = 0; i < SID_INDEX; i++)
    sector_deallocate_disk (inode->data.arr[i], 0);
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
      
      cache_sector_write (sector_idx, buffer + bytes_written,
                          chunk_size, sector_ofs);
    
      // POTENTIAL SYNCH ISSUE
      if (offset + chunk_size > inode->data.length)
        {
          inode->data.length = offset + chunk_size;
          inode_write_to_table (inode->inumber, &inode->data);
        }
      
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
