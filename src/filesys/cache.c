#include "filesys/cache.h"
#include <debug.h>
#include <string.h>
#include "devices/block.h"
#include "devices/timer.h"
#include "filesys/filesys.h"
#include "lib/kernel/bitmap.h"
#include "lib/kernel/hash.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/thread.h"

#define NUM_CACHE_SECTORS 64

struct list cache_free_list;
struct hash cache_closed_hash;
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

struct fetch_struct
  {
    block_sector_t sector;
    struct list_elem elem;
  };

static struct cache_entry cache_entries[NUM_CACHE_SECTORS];
static struct lock cache_lock;
static struct semaphore cache_fetch_sem;
static struct list cache_fetch_list;
static unsigned cur_index;
static bool done;

static struct cache_entry *cache_lookup (unsigned, bool);
static void cache_flush (void);
static void cache_flush_loop (void *);
static void cache_fetch_loop (void *);
static void cache_sector_cr (unsigned, bool);
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
  success = hash_init (&cache_closed_hash,
                       cache_hash_func,
                       cache_less_func,
                       NULL);
  ASSERT (success);
  lock_init (&cache_lock);
  list_init (&cache_free_list);
  list_init (&cache_fetch_list);
  sema_init (&cache_fetch_sem, 0);
  cur_index = 0;
  for (unsigned i = 0; i < NUM_CACHE_SECTORS; i++)
    {
      struct cache_entry *ce = &cache_entries[i];
      lock_init (&ce->lock);
      list_push_back (&cache_free_list, &ce->list_elem);
    }

  done = false;
  thread_create ("cache_loop", PRI_MAX, cache_flush_loop, NULL);
  thread_create ("fetch_loop", PRI_MAX, cache_fetch_loop, NULL);
}

/* Writes dirty sectors and frees all resources. */
void 
cache_destroy (void)
{
  cache_flush ();
  done = true;
  hash_destroy (&cache_hash, NULL);
  hash_destroy (&cache_closed_hash, NULL);
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
cache_sector_write (unsigned sector, const void *buffer,
                    unsigned size, unsigned offset)
{
  ASSERT (offset + size <= BLOCK_SECTOR_SIZE);
  struct cache_entry *ce = cache_get_entry (sector);
  ce->accessed = true;
  ce->dirty = true;
  memcpy (&ce->data[offset], buffer, size);
  lock_release (&ce->lock);
}

/* Schedules an asynchronous fetch of block sector SECTOR and returns
   immediately. */
void
cache_sector_fetch_async (unsigned sector)
{
  struct fetch_struct *fs = malloc (sizeof (struct fetch_struct));
  /* Being unable to prefetch is not critical. */
  if (fs == NULL)
    return;
  fs->sector = sector;
  list_push_back (&cache_fetch_list, &fs->elem);
  sema_up (&cache_fetch_sem);
} 

/* Checks is a sector is in the cache. If so, it moves it from
   CACHE_HASH to CACHE_CLOSED_HASH. */
void
cache_sector_close (unsigned sector)
{
  cache_sector_cr (sector, false);
}

/* Adds a dirty cache_entry for SECTOR, as a blank sector. Used
   when allocating sectors to a file. The entry is marked dirty. */
void
cache_sector_add (unsigned sector)
{
  struct cache_entry *ce = cache_get_entry (sector);
  ce->accessed = true;
  ce->dirty = true;
  memset (&ce->data[0], 0, BLOCK_SECTOR_SIZE);
  lock_release (&ce->lock);
}

/* Used when freeing a sector, to prevent a deleted sector that is only
   present in cache from ever being writtien to the disk. This is just
   like closing a sector, but we set dirty to false, and free it. */
void
cache_sector_remove (unsigned sector)
{
  cache_sector_cr (sector, true);
}

static void
cache_sector_cr (unsigned sector, bool deleted)
{
  lock_acquire (&cache_lock);
  struct cache_entry *ce = cache_lookup (sector, false);
  if (ce != NULL)
    {
      hash_delete (&cache_hash, &ce->hash_elem);
      hash_insert (&cache_closed_hash, &ce->hash_elem);
      if (deleted)
        {
          ce->dirty = false;
          /* Code to free sector */
        }
    }
  lock_release (&cache_lock);
}

/* Returns a pointer to the cache_entry assigned to SECTOR, if present.
   Otherwise returns NULL. */
static struct cache_entry *
cache_lookup (unsigned sector, bool closed)
{
  ASSERT (lock_held_by_current_thread (&cache_lock));
  struct hash *hash = closed ? &cache_closed_hash : &cache_hash;
  struct cache_entry_stub ces;
  ces.sector = sector;
  struct hash_elem *e = hash_find (hash, &ces.elem);
  if (e == NULL)
    return NULL;

  return hash_entry (e, struct cache_entry, hash_elem);
}

/* First checks CACHE_CLOSED_HASH, and evicts an entry from it if
   it is not empty. Otherwise uses clock algorithm to select a 
   cache_entry for eviction, removes the entry from CACHE_HASH 
   (signifies not present in cache). */
static struct cache_entry *
cache_evict (void)
{
  struct cache_entry *ce;
  struct hash_iterator i;
  hash_first (&i, &cache_closed_hash);
  struct hash_elem *e = hash_next (&i);
  if (e != NULL)
    {
      hash_delete (&cache_closed_hash, e);
      ce = hash_entry (e, struct cache_entry, hash_elem);
      lock_acquire (&ce->lock);
      return ce;
    } 
  
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

/* Waits for prefetch requests to be queued, then fetches the requested
   sectors before waiting again. Should be run in a high priority thread
   to ensure executation before CACHE_SECTOR_PREFETCH_ASYNC's caller
   attempts to fetch itself. */
static void
cache_fetch_loop (void * aux UNUSED)
{
  while (true)
    {
      sema_down (&cache_fetch_sem);
      struct list_elem *e = list_pop_front (&cache_fetch_list);
      struct fetch_struct *fs = list_entry (e, struct fetch_struct, elem);
      struct cache_entry *ce = cache_get_entry (fs->sector);
      free (fs);
      lock_release (&ce->lock);
    }
}

/* Writes back all dirty cache_entries to the disk and marks them
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

/* Thread function that flushes the cache every 30 seconds. This
   should run at high priority to ensure timely flushes. */
static void
cache_flush_loop (void *aux UNUSED)
{
  while (!done)
    {
      cache_flush ();
      timer_msleep (30 * 1000);
    }
}

/* Searches for SECTOR in the cache. If not present, and cache_entry is 
   assigned, evicting and writing back an entry if necessary. Data is 
   read from the disk to the cache_entry, and a pointer to the entry 
   is returned. */
static struct cache_entry *
cache_get_entry (unsigned sector)
{
  struct cache_entry *ce;
  lock_acquire (&cache_lock);
  ce = cache_lookup (sector, false);
  if (ce != NULL)
    {
      lock_acquire (&ce->lock);
      lock_release (&cache_lock);
      return ce;
    }
  /* May still be intact in CACHE_CLOSED_HASH. */
  ce = cache_lookup (sector, true);
  if (ce != NULL)
    {
      hash_delete (&cache_closed_hash, &ce->hash_elem);
      hash_insert (&cache_hash, &ce->hash_elem);
      lock_acquire (&ce->lock);
      lock_release (&cache_lock);
      return ce;
    }
  /* Sector must be read from the disk. */
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
