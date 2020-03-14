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
// TO DO: MAYBE I CANT KEEP THE ARR FRO INODE DISK SINC EIT COUNTS AS METADATA.
// IN THIS CASE, KEEP IT JUST ON DISK, MAKE INODE TABLE V HIGHPRIORITY FOR STAYING
// IN CACHE

/* On-disk inode. Must not be larger than BLOCK_SECTOR_SIZE. */
struct inode_disk
  {
    bool in_use;                        /* Inode entry in use. */
    off_t length;                       /* File size in bytes. */
    unsigned arr[8];                    /* Direct, indirect and doubly
                                           inderect sector mappings. */
  };

/* In-memory inode. */
struct inode 
  {
    struct list_elem elem;              /* Element in inode list. */
    unsigned inumber;                   /* inumber in inode table. */
    struct lock open_lock;
    int open_cnt;                       /* Number of openers. */
    bool removed;                       /* True if deleted, false otherwise. */
    struct lock deny_write_lock;
    int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
    struct lock data_lock;
  };

/* Keeps state while sequentially initializing
   an inode. Used for INODE_CREATE_SEQ (). */
struct create_seq_state
  {
    struct inode *inode;
    block_sector_t cur_sector;
    block_sector_t start_sector;
    size_t file_sectors;
    size_t *total_sectors;
    struct inode_disk data;
  };

#define INODES_PER_SECTOR (BLOCK_SECTOR_SIZE / sizeof (struct inode_disk))

static unsigned inumber_to_ofs (inumber_t);
static block_sector_t inumber_to_sector (inumber_t);
static void inode_read_from_table (inumber_t, struct inode_disk *);
static void inode_write_to_table (inumber_t, struct inode_disk *);
static inline size_t bytes_to_sectors (off_t);
static bool create_seq_advance_sector (block_sector_t *, 
                                       struct create_seq_state *,
                                       unsigned);
static block_sector_t byte_to_sector (struct inode *, off_t);
static void inode_release_sectors (struct inode *);

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

static struct lock open_inodes_lock;

static struct lock inumber_lock;

/* Initializes the inode module. */
void
inode_init (void) 
{
  list_init (&open_inodes);
  lock_init (&inumber_lock);
  lock_init (&open_inodes_lock);
}

/* Allocates the first available inumber from the inode table,
   and places it at INUMBER. Returns false if no inumbers are
   available (maximum number of files reached). */
bool
inode_assign_inumber (inumber_t *inumber)
{
  struct inode_disk disk_inode;
  for (unsigned i = 0; i < INODE_TABLE_SECTORS; i++)
    {
      for (unsigned j = 0; j < INODES_PER_SECTOR; j++)
        {
          lock_acquire (&inumber_lock);
          cache_sector_read (i, &disk_inode, sizeof disk_inode, 
                             j * sizeof disk_inode);
          if (!disk_inode.in_use)
            {
              disk_inode.in_use = true;
              cache_sector_write (i, &disk_inode, sizeof disk_inode, 
                                  j * sizeof disk_inode);
              lock_release (&inumber_lock);
              *inumber = i * INODES_PER_SECTOR + j;
              return true;
            }
          lock_release (&inumber_lock);
        }
    }

  return false;
}

/* Releases INUMBER from the inode table. */
void
inode_release_inumber (inumber_t inumber)
{
  struct inode_disk disk_inode;
  memset (&disk_inode, 0, sizeof (disk_inode));
  inode_write_to_table (inumber, &disk_inode);
}

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create (inumber_t inumber, off_t length)
{
  ASSERT (length >= 0);

  struct inode_disk disk_inode; 
  
  disk_inode.length = length;
  disk_inode.in_use = true;
  memset (&disk_inode.arr, 0, sizeof (disk_inode.arr));
  inode_write_to_table (inumber, &disk_inode);
  return true; //THIS CANT FAIL
}

/* Creates an inode for a file of length LENGTH, sequentially assigning 
   sectors starting at START_SECTOR. Returns the populates SECTOR with 
   the number of sectors used, including metadata sectors. The caller is
   responsible for allocating the sectors used. */

bool
inode_create_seq (inumber_t inumber, size_t *sectors, 
                  size_t length, unsigned start_sector)
{
  inode_create (inumber, length);

  struct create_seq_state s;
  s.file_sectors = bytes_to_sectors (length);
  s.inode = inode_open (inumber);
  s.start_sector = start_sector;
  s.cur_sector = start_sector;
  s.total_sectors = sectors;
  s.data.length = length;
  memset (&s.data.arr, 0, sizeof s.data.arr);

  unsigned i = 0;
  for (; i < SID_INDEX; i++)
    if (create_seq_advance_sector (&s.data.arr[i], &s, 0))
      return true;
  for (; i < DID_INDEX; i++)
    if (create_seq_advance_sector (&s.data.arr[i], &s, 1))
      return true;
  for (; i < MAX_INDEX; i++)
    if (create_seq_advance_sector (&s.data.arr[i], &s, 2))
      return true;

  inode_close (s.inode);
  return false;
}
// TEST THIS FOR LARGE FREE MAPS 

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (inumber_t inumber)
{
  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */
  lock_acquire (&open_inodes_lock);
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e)) 
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->inumber == inumber) 
        {
          inode_reopen (inode);
          lock_release (&open_inodes_lock);
          return inode; 
        }
    }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
    {
      lock_release (&open_inodes_lock);
      return NULL;
    }

  /* Initialize. */
  list_push_front (&open_inodes, &inode->elem);
  lock_init (&inode->open_lock);
  lock_init (&inode->deny_write_lock);
  lock_init (&inode->data_lock);
  /* Prevent a premature reopen. */
  lock_acquire (&inode->open_lock);
  lock_release (&open_inodes_lock);
  inode->inumber = inumber;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  
  inode->open_cnt = 1;
  lock_release (&inode->open_lock);
  return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  if (inode != NULL)
    {
      lock_acquire (&inode->open_lock);
      inode->open_cnt++;
      lock_release (&inode->open_lock);
    }
  return inode;
}

/* Returns INODE's inode number. */
inumber_t
inode_get_inumber (const struct inode *inode)
{
  return inode->inumber;
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
  lock_acquire (&inode->open_lock);
  bool last = --inode->open_cnt == 0;
  lock_release (&inode->open_lock);
  if (last)
    {
      /* Remove from inode list and release lock. */
      lock_acquire (&open_inodes_lock);
      list_remove (&inode->elem);
      lock_release (&open_inodes_lock);
      /* Deallocate blocks if removed. */
      if (inode->removed) 
        {
          inode_release_sectors (inode);
          inode_release_inumber (inode->inumber);
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
  
  /* Read ahead. */
  /* Offset at start of next bock. */
  offset -= offset % BLOCK_SECTOR_SIZE;
  // DONT NEED LOCK HERE IF STORE OF AN UNSIGNED IN MEMCPY IS ATOMIC
  struct inode_disk disk_inode;
  inode_read_from_table (inode->inumber, &disk_inode);
  if (offset < disk_inode.length)
    {
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      cache_sector_fetch_async (sector_idx);
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
    
      lock_acquire (&inode->data_lock);
      struct inode_disk disk_inode;
      inode_read_from_table (inode->inumber, &disk_inode);
      if (offset + chunk_size > disk_inode.length)
        {
          disk_inode.length = offset + chunk_size;
          inode_write_to_table (inode->inumber, &disk_inode);
        }
      lock_release (&inode->data_lock);
      
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
  lock_acquire (&inode->deny_write_lock);
  inode->deny_write_cnt++;
  lock_release (&inode->deny_write_lock);
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
 
  lock_acquire (&inode->deny_write_lock);
  inode->deny_write_cnt--;
  lock_release (&inode->deny_write_lock);
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode)
{
  struct inode_disk disk_inode;
  inode_read_from_table (inode->inumber, &disk_inode);
  return disk_inode.length;
}

/* Returns the sector offset inode INUMBER is stored at. */
static inline unsigned 
inumber_to_ofs (inumber_t inumber)
{
  return (inumber % INODES_PER_SECTOR) * sizeof (struct inode_disk);
}

/* Returns the sector number inode INUMBER is stored at. */
static inline block_sector_t
inumber_to_sector (inumber_t inumber)
{
  return inumber / INODES_PER_SECTOR;
}

static void
inode_read_from_table (inumber_t inumber, struct inode_disk *disk_inode)
{
  block_sector_t table_sector = inumber_to_sector (inumber);
  off_t table_ofs = inumber_to_ofs (inumber);
  ASSERT (table_sector < INODE_TABLE_SECTORS);
  cache_sector_read (table_sector, disk_inode, sizeof *disk_inode,
                     table_ofs);
}
 
/* Writes DISK_INODE to the inode table at index INUMBER. */
static void
inode_write_to_table (inumber_t inumber, struct inode_disk *disk_inode)
{ 
  block_sector_t table_sector = inumber_to_sector (inumber);
  off_t table_ofs = inumber_to_ofs (inumber);
  ASSERT (table_sector < INODE_TABLE_SECTORS);
  cache_sector_write (table_sector, disk_inode, sizeof *disk_inode, 
                      table_ofs);
}

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, BLOCK_SECTOR_SIZE);
}

/* Advances the state while sequentially intializing an
   inode. If the initialization is done, the inode is
   written to the disk, and the function returns true.
   Otherwise returns false. */
static bool
create_seq_advance (struct create_seq_state *s)
{
  if (--s->file_sectors == 0)
    {
      inode_write_to_table (s->inode->inumber, &s->data);
      inode_close (s->inode);
      *s->total_sectors = s->cur_sector - s->start_sector;
      return true;
    }
  s->cur_sector++;
  return false;
}

/* Assigns all sectors associated with a root sector to be stored at
   SAVED_SECTOR, with level of indirection of DEPTH. Returns true if
   inode initialization is complete, false otherwise. */
static bool
create_seq_advance_sector (block_sector_t *saved_sector, 
                           struct create_seq_state *s, unsigned depth)
{
  *saved_sector = s->cur_sector++;
  if (depth == 0)
    return create_seq_advance (s);
  for (unsigned i = 0; i < NUM_PER_SECTOR; i++)
    {
      cache_sector_write (*saved_sector, &s->cur_sector, 
                          sizeof (block_sector_t), 
                          i * sizeof (block_sector_t));
      block_sector_t saved_sector_new = s->cur_sector;
      if (create_seq_advance_sector (&saved_sector_new, s, depth))
        return true;
    }
  return false;
}
          

static bool
sector_fixup_arr (struct inode *inode, struct inode_disk *disk_inode,
                  size_t index)
{
  lock_acquire (&inode->data_lock);
  inode_read_from_table (inode->inumber, disk_inode);
  if (disk_inode->arr[index] == 0)
    {
      if (!free_map_allocate (1, &disk_inode->arr[index]))
        {
          lock_release (&inode->data_lock);
          return false;
        }
      cache_sector_add (disk_inode->arr[index]);
      inode_write_to_table (inode->inumber, disk_inode);
    }
  lock_release (&inode->data_lock);
  return true;
}
  


/* Gets a sector number from an index sector FROM_SECTOR,
   at index INDEX. Allocates and writes back a sector number 
   if that index of FROM_SECTOR had not previously been allocated.
   Return false if a sector allocation fails. */
static bool
sector_fixup_disk (block_sector_t from_sector, block_sector_t *to_sector, 
                   size_t index)
{
  /* Slightly heavy-handed to lock the entire sector, but we need
     to prevent double allocations for the same index in a sector. */
  cache_sector_lock (from_sector);
  cache_sector_read (from_sector, to_sector, sizeof (unsigned), 
                     index * sizeof (unsigned));
  if (*to_sector == 0)
    {
      if (!free_map_allocate (1, to_sector))
        {
          cache_sector_unlock (from_sector);
          return false;
        }
      cache_sector_add (*to_sector);
      cache_sector_write (from_sector, to_sector, sizeof (unsigned), 
                          index * sizeof (unsigned));
    }
  cache_sector_unlock (from_sector);
  return true;
}

/* Allocates a sector of index INDEX if not already allocated. 
   Allocates intermediate sectors as needed based on level of 
   indirection (DEPTH), if they have not already been allocated.
   Returns the final sector number, and returns -1 if a necessary
   sector allocation fails. */
static block_sector_t
sector_fixup_depth (struct inode *inode, block_sector_t start_index, 
                    size_t prev_limit, size_t index, unsigned depth)
{
  index -= prev_limit;
  unsigned arr_index  = index;
  for (unsigned i = 0; i < depth; i++)
    arr_index /= NUM_PER_SECTOR;
  
  arr_index += start_index;
  struct inode_disk disk_inode;
  if (!sector_fixup_arr (inode, &disk_inode, arr_index))
    return -1;
  
  unsigned sector_to = disk_inode.arr[arr_index];
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

  return sector_to;
}
      
// HOW YO FREE UNUSED PAGES ON RETURN?

/* Returns the block device sector that contains byte offset POS
   within INODE. Allocates a sector if needed. Returns -1 if POS 
   excdeeds the maximum file size, or if a sector
   allocation was needed but no sectors were free. */
static block_sector_t
byte_to_sector (struct inode *inode, off_t pos)
{
  ASSERT (inode != NULL);
  // NEED TO LOCK INDEX, to not want to block other accesses on IO
   // fuck. What if different indeces need same block
  unsigned index = pos / BLOCK_SECTOR_SIZE;
  if (index < DIRECT_LIMIT)
    return sector_fixup_depth (inode, 0, 0, index, 0);
  else if (index < SID_LIMIT)
    return sector_fixup_depth (inode, SID_INDEX, DIRECT_LIMIT, index, 1);
  else if (index < DID_LIMIT)
    return sector_fixup_depth (inode, DID_INDEX, SID_LIMIT, index, 2);  
 
  return -1;
}

/* Releases a sector and removes it from the buffer cache.
   0 is not a valid file sector and will not be deallocated. */
static void
sector_deallocate (block_sector_t sector)
{
  if (sector != 0)
    {
      cache_sector_remove (sector);
      free_map_release (sector, 1);
    }
}

/* Releases FROM_SECTOR and all sectors it references, to depth
   DEPTH. */
static void
sector_deallocate_disk (block_sector_t from_sector, unsigned depth)
{
  /* Skip unassigned entries. */
  if (from_sector == 0)
    return;
  if (depth-- == 0)
    {
      sector_deallocate (from_sector);
      return;
    }

  block_sector_t to_sector;
  for (unsigned j = 0; j < NUM_PER_SECTOR; j++)
    {
      cache_sector_read (from_sector, &to_sector, sizeof (block_sector_t),
                         j * sizeof (block_sector_t));
      sector_deallocate_disk (to_sector, depth);
    }
  sector_deallocate (from_sector);
}

/* Releases all sectors associated with INODE.
   */
static void
inode_release_sectors (struct inode *inode)
{
  struct inode_disk disk_inode;
  inode_read_from_table (inode->inumber, &disk_inode);
  for (unsigned i = 0; i < SID_INDEX; i++)
    sector_deallocate_disk (disk_inode.arr[i], 0);
  for (unsigned i = SID_INDEX; i < DID_INDEX; i++)
    sector_deallocate_disk (disk_inode.arr[i], 1);
  for (unsigned i = DID_INDEX; i < MAX_INDEX; i++)
    sector_deallocate_disk (disk_inode.arr[i], 2);
}
