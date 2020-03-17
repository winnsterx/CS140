#include "filesys/free-map.h"
#include <bitmap.h>
#include <debug.h>
#include <round.h>
#include "filesys/cache.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "filesys/inode.h"
#include "threads/malloc.h"
#include "threads/synch.h"

static struct bitmap *free_map;      /* Free map, one bit per sector. */
static void *buf;
static size_t bit_cnt;
static size_t buf_size;
size_t num_sectors;
static struct lock DEBUG_FREEMAP_LOCK;

/* Initializes the free map. */
void
free_map_init (void) 
{
  lock_init (&DEBUG_FREEMAP_LOCK);
  bit_cnt = block_size (fs_device);
  buf_size = bitmap_buf_size (bit_cnt);
  num_sectors = DIV_ROUND_UP (bitmap_buf_size (bit_cnt),
                                       BLOCK_SECTOR_SIZE);
}

/* Allocates CNT consecutive sectors from the free map and stores
   the first into *SECTORP.
   Returns true if successful, false if not enough consecutive
   sectors were available or if the free_map file could not be
   written. */
bool
free_map_allocate (size_t cnt, block_sector_t *sectorp)
{
  block_sector_t sector = bitmap_scan_and_flip (free_map, 0, cnt, false);
  if (sector != BITMAP_ERROR)
    {
      *sectorp = sector;
      cache_sector_dirty_external (INODE_TABLE_SECTORS);
    }
  return sector != BITMAP_ERROR;
}

/* Makes CNT sectors starting at SECTOR available for use. */
void
free_map_release (block_sector_t sector, size_t cnt)
{
  ASSERT (bitmap_all (free_map, sector, cnt));
  bitmap_set_multiple (free_map, sector, cnt, false);
  cache_sector_dirty_external (INODE_TABLE_SECTORS);
}

/* Opens the free map file and reads it from disk. */
void
free_map_open (void) 
{
  buf = malloc (bitmap_buf_size (bit_cnt));
  if (buf == NULL)
    PANIC ("bitmap creation failed--file system device is too large");  
 
  if (!cache_sector_read_external (INODE_TABLE_SECTORS, buf,
                                   num_sectors * BLOCK_SECTOR_SIZE)) 
    PANIC ("Could not cache the free-map.");
  free_map = bitmap_open_in_buf (bit_cnt, buf, buf_size);
}

/* Writes the free map to disk and closes the free map file. */
void
free_map_close (void) 
{
  cache_sector_free_external (INODE_TABLE_SECTORS);
  free (buf);
}

/* Creates a new free map file on disk and writes the free map to
   it. */
void
free_map_create (void) 
{
  buf = calloc (bitmap_buf_size (bit_cnt), 1);
  if (buf == NULL)
    PANIC ("bitmap creation failed--file system device is too large");  
  if (!cache_sector_read_external (INODE_TABLE_SECTORS, buf,
                                   num_sectors * BLOCK_SECTOR_SIZE)) 
    PANIC ("Could not cache the free-map.");
  
  free_map = bitmap_create_in_buf (bit_cnt, buf, buf_size);

  /* Mark all sectors used by the inode table and free-map. */
  for (unsigned i = 0; i < INODE_TABLE_SECTORS + num_sectors; i++)
    bitmap_mark (free_map, i);
  cache_sector_dirty_external (INODE_TABLE_SECTORS); 
}
