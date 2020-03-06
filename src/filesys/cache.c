#include "filesys/cache.h"
#include <debug.h>
#include <string.h>
#include "devices/block.h"
#include "devices/timer.h"
#include "filesys/filesys.h"
#include "lib/kernel/bitmap.h"
#include "lib/kernel/hash.h"
#include "threads/synch.h"
#include "threads/thread.h"

#define NUM_CACHE_SECTORS 64

struct list cache_free_list;
struct list cache_evict_list;
struct hash cache_hash;

/* Used for hash searches without allocating an entire
   cache_entry. Must have same offset between SECTOR and
   ELEM as a cache_entry. */
struct cache_entry_stub
  {
    unsigned sector;
    struct hash_elem elem;
  };

struct cache_entry
  {
    unsigned sector;
    union
      {
        struct hash_elem hash_elem;
        struct list_elem list_elem;
      };
    struct lock lock;
    bool accessed;
    bool dirty;
    bool is_meta;
    uint8_t data[BLOCK_SECTOR_SIZE];
  };

static struct cache_entry cache_entries[NUM_CACHE_SECTORS];
static struct lock cache_lock;
static unsigned cur_index;
static bool done;

static void cache_flush (void);
static void cache_flush_loop (void *);
static struct cache_entry *cache_get_entry (unsigned);
static unsigned cache_hash_func (const struct hash_elem *, void *);
static bool cache_less_func (const struct hash_elem *, 
                             const struct hash_elem *, void *);

/* Sets up initial list of free cache_entries, allocates CACHE_HASH. 
   Initializes CACHE_LOCK, launches thread that periodically flushes
   cache. */
void 
cache_init (void)
{
  bool success = hash_init (&cache_hash, 
                            cache_hash_func, 
                            cache_less_func, 
                            NULL);
  ASSERT (success);
  lock_init (&cache_lock);
  list_init (&cache_free_list);
  cur_index = 0;
  for (unsigned i = 0; i < NUM_CACHE_SECTORS; i++)
    {
      struct cache_entry *ce = &cache_entries[i];
      lock_init (&ce->lock);
      list_push_back (&cache_free_list, &ce->list_elem);
    }

  done = false;
  thread_create ("cache_loop", PRI_DEFAULT, cache_flush_loop, (void *) &done);
}

/* Writes dirty sectors and frees all resources. */
void 
cache_destroy (void)
{
  cache_flush ();
  done = true;
  hash_destroy (&cache_hash, NULL);
} 

/* If the SECTOR is not present in the cache, SECTOR is read into the cache.
   SIZE bytes from OFFSET in the cache slot are copied into BUFFER. The
   cache slot is marked as accessed. */
void 
cache_sector_read (unsigned sector, void *buffer,
                   unsigned size, unsigned offset)
{
  ASSERT (offset + size <= BLOCK_SECTOR_SIZE);
  struct cache_entry *ce = cache_get_entry (sector);
  ce->accessed = true;
  memcpy (buffer, &ce->data[offset], size);
  lock_release (&ce->lock);
}

/* If the SECTOR is not present in the cache, SECTOR is read into the cache.
   SIZE bytes from BUFFER are copied into OFFSET bytes into the cache_entry.
   The cache_entry is marked as accessed and dirty. */
void 
cache_sector_write (unsigned sector, void *buffer,
                    unsigned size, unsigned offset)
{
  ASSERT (offset + size <= BLOCK_SECTOR_SIZE);
  struct cache_entry *ce = cache_get_entry (sector);
  ce->accessed = true;
  ce->dirty = true;
  memcpy (&ce->data[offset], buffer, size);
  lock_release (&ce->lock);
}

/* Returns a pointer to the cache_entry assigned to SECTOR, if present.
   Otherwise returns NULL. */
static struct cache_entry *
cache_lookup (unsigned sector)
{
  ASSERT (lock_held_by_current_thread (&cache_lock));
  struct cache_entry_stub ces;
  ces.sector = sector;
  struct hash_elem *e = hash_find (&cache_hash, &ces.elem);
  if (e == NULL)
    return NULL;

  return hash_entry (e, struct cache_entry, hash_elem);
}

/* Uses clock algorithm to select a cache_entry for eviction, removes
   the entry from CACHE_HASH (signifies not present in cache). */
static struct cache_entry *
cache_evict (void)
{
  struct cache_entry *ce;
  while (true)
    {
      cur_index++;
      cur_index %= NUM_CACHE_SECTORS;
      ce = &cache_entries[cur_index];
      
      if (lock_try_acquire (&ce->lock))
        {
          if (ce->accessed)
            {
              ce->accessed = false;
              lock_release (&ce->lock);
            }
          else
            break;
        }
    }
  
  hash_delete (&cache_hash, &ce->hash_elem);
  return ce;
}

/* Writes back all dirty cache_entries to the disc and marks them
   clean. */
static void
cache_flush (void)
{
  for (unsigned i = 0; i < NUM_CACHE_SECTORS; i++)
    {
      struct cache_entry *ce = &cache_entries[i];
      lock_acquire (&ce->lock);
      if (ce->dirty)
        {
          block_write (fs_device, ce->sector, &ce->data);
          ce->dirty = false;
        }
      lock_release (&ce->lock);
    }
}

/* Thread function that flushes the cache every 30 seconds. */
static void
cache_flush_loop (void *done)
{
  while (!*(bool *) done)
    {
      cache_flush ();
      timer_msleep (30 * 1000);
    }
}

/* Searches for SECTOR in the cache. If not present, and cache_entry is 
   assigned, evicting and writing back an entry if necessary. Data is 
   read from the disc to the cache_entry, and a pointer to the entry 
   is returned. */
static struct cache_entry *
cache_get_entry (unsigned sector)
{
  struct cache_entry *ce;
  lock_acquire (&cache_lock);
  ce = cache_lookup (sector);
  if (ce != NULL)
    {
      lock_acquire (&ce->lock);
      lock_release (&cache_lock);
      return ce;
    }
  /* Sector must be read from the disc. */
  unsigned old_sector;
  bool write_back = false;
  if (list_empty (&cache_free_list))
    {
      ce = cache_evict ();
      if (ce->dirty)
        {
          old_sector = ce->sector;
          write_back = true;
        }
    }
  else
    {
      ce = list_entry (list_pop_front (&cache_free_list), 
                       struct cache_entry, 
                       list_elem);
      /* Unlocked when read/write is complete. */
      lock_acquire (&ce->lock);
    }
  ce->sector = sector;
  hash_insert (&cache_hash, &ce->hash_elem);
  lock_release (&cache_lock);
  if (write_back)
    block_write (fs_device, old_sector, &ce->data);
  ce->dirty = false;
  block_read (fs_device, sector, &ce->data);
  return ce;
}

/* Hashes cache_entries by the sector number it stores. */
static unsigned 
cache_hash_func (const struct hash_elem *e, void *aux UNUSED)
{
  struct cache_entry *ce = hash_entry (e, struct cache_entry, hash_elem);
  return hash_int ((int) ce->sector);
}

/* Comparator for CACHE_HASH. */
static bool 
cache_less_func (const struct hash_elem *a,
                 const struct hash_elem *b,
                 void *aux UNUSED)
{
  struct cache_entry *cea = hash_entry (a, struct cache_entry, hash_elem);
  struct cache_entry *ceb = hash_entry (b, struct cache_entry, hash_elem);
  return cea->sector < ceb->sector;
}
