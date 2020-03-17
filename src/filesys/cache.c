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
#include "devices/shutdown.h"



#define NUM_CACHE_SECTORS 64

struct hash cache_closed_hash;
struct hash cache_hash;
struct list list_ext;
struct lock evict_lock;


/* Used for hash searches without allocating an entire
   cache_entry. Must have same offset between SECTOR and
   ELEM as a cache_entry. */
struct cache_entry_stub
  {
    block_sector_t sector;
    struct hash_elem elem;
  };

struct cache_entry
  {
    block_sector_t sector;
    struct hash_elem elem;
    struct rw_lock rw_lock;
    uint8_t accessed;
    bool dirty;
    bool is_meta;
    uint8_t data[BLOCK_SECTOR_SIZE];
  };

struct cache_entry_ext
  {
    block_sector_t sector;
    struct list_elem ext_elem;
    bool dirty;
    unsigned num_sectors;
    void *data;
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

static struct cache_entry *cache_get_current_locked_entry (unsigned);
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
   cache, as well as the thread that handles prefetch requests. */
void 
cache_init (void)
{
  if (!hash_init (&cache_hash, 
                  cache_hash_func, 
                  cache_less_func, 
                  NULL) ||
      !hash_init (&cache_closed_hash,
                  cache_hash_func,
                  cache_less_func,
                  NULL))
    {
      PANIC ("Could not initialize the filesys cache.");
    }

  lock_init (&cache_lock);
  lock_init (&evict_lock);
  list_init (&cache_fetch_list);
  list_init (&list_ext);
  sema_init (&cache_fetch_sem, 0);
  cur_index = 0;

  for (unsigned i = 0; i < NUM_CACHE_SECTORS; i++)
    {
      /* Initially place placeholder sectors in CLOSED_HASH. These
         will be evicted until the cache is full. */
      struct cache_entry *ce = &cache_entries[i];
      rw_lock_init (&ce->rw_lock);
      ce->sector = -1;
      ce->dirty = false;
      hash_insert (&cache_closed_hash, &ce->elem);
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
                   unsigned size, unsigned offset, unsigned pri)
{
  ASSERT (offset + size <= BLOCK_SECTOR_SIZE);
  struct cache_entry *ce = cache_get_entry (sector);
  ce->accessed = pri;
  memcpy (buffer, &ce->data[offset], size);
  if (!rw_lock_held_by_current_thread_w (&ce->rw_lock))
    rw_lock_release_r (&ce->rw_lock);
}

/* If the SECTOR is not present in the cache, SECTOR is read into the cache.
   SIZE bytes from BUFFER are copied into OFFSET bytes into the cache_entry.
   The cache_entry is marked as accessed and dirty. */
void 
cache_sector_write (unsigned sector, const void *buffer,
                    unsigned size, unsigned offset, unsigned pri)
{
  ASSERT (offset + size <= BLOCK_SECTOR_SIZE);
  struct cache_entry *ce = cache_get_entry (sector);
  ce->accessed = pri;
  ce->dirty = true;
  memcpy (&ce->data[offset], buffer, size);
  if (!rw_lock_held_by_current_thread_w (&ce->rw_lock))
    rw_lock_release_r (&ce->rw_lock);
}

/* Locks a sector in the sector cannot be evicted or accessed while
   locked. Keep sectors locked for as short a time as possible,
   as a locked sector can stall period cache flushing. */
void
cache_sector_lock (unsigned sector)
{
  /* Thread takes a read lock, which must be promoted. */
  struct cache_entry *ce = cache_get_entry (sector);
  rw_lock_promote (&ce->rw_lock);
  thread_current ()->locked_ce = ce;
}

/* Unlocks a sector. The sector is then accessible and evictable. */
void
cache_sector_unlock (unsigned sector)
{
  struct cache_entry *ce = thread_current ()->locked_ce;
  ASSERT (ce->sector == sector);
  rw_lock_release_w (&ce->rw_lock);
  thread_current ()->locked_ce = NULL;
}

/* Adds a dirty cache_entry for SECTOR, as a blank sector. Used
   when allocating sectors to a file. The entry is marked dirty. */
void
cache_sector_add (unsigned sector, unsigned pri)
{
  struct cache_entry *ce = cache_get_entry (sector);
  ce->accessed = pri;
  ce->dirty = true;
  memset (&ce->data[0], 0, BLOCK_SECTOR_SIZE);
  if (!rw_lock_held_by_current_thread_w (&ce->rw_lock))  
    rw_lock_release_r (&ce->rw_lock);
}

/* Schedules an asynchronous fetch of block sector SECTOR and returns
   immediately. Should not be called on a locked sector. Does nothing
   if a fetch_struct could not be allocated. */
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

/* Marks a sector as closed, making it considered for eviction
   earlier. */
void
cache_sector_close (unsigned sector)
{
  cache_sector_cr (sector, false);
}

/* Closes a sector, making it considered earlier for eviction. Marks
   the sector as clean, so the sector will not be written to the disk
   on eviction. */
void
cache_sector_remove (unsigned sector)
{
  cache_sector_cr (sector, true);
}

bool
cache_sector_read_external (unsigned sector, void *buf, unsigned size)
{
  ASSERT (size % BLOCK_SECTOR_SIZE == 0);
  struct cache_entry_ext *ce = malloc (sizeof (struct cache_entry_ext));
  if (ce == NULL)
    return false;
  ce->num_sectors = size / BLOCK_SECTOR_SIZE;
  ce->data = buf;
  ce->dirty = false;
  for (unsigned i = 0; i < ce->num_sectors; i++)
    block_read (fs_device, sector + i, buf + i * BLOCK_SECTOR_SIZE);
  list_push_back (&list_ext, &ce->ext_elem);
  return true;
}

void
cache_sector_free_external (unsigned sector)
{
  bool found;
  struct list_elem *e;
  struct cache_entry_ext *ce;
  for (e = list_begin (&list_ext); e != list_end (&list_ext); 
       e = list_next (e))
    {
      ce = list_entry (e, struct cache_entry_ext, ext_elem);
      if (ce->sector = sector)
        {
          found = true;
          break;
        }
    }
  ASSERT (found);
  if (ce->dirty)
    for (unsigned i = 0; i < ce->num_sectors; i++)
      block_write (fs_device, sector + i, ce->data + i * BLOCK_SECTOR_SIZE);
  list_remove (&ce->ext_elem);
  free (ce);
}

void
cache_sector_dirty_external (unsigned sector)
{
  struct list_elem *e;
  struct cache_entry_ext *ce;
  for (e = list_begin (&list_ext); e != list_end (&list_ext); 
       e = list_next (e))
    {
      ce = list_entry (e, struct cache_entry_ext, ext_elem);
      if (ce->sector = sector)
        ce->dirty = true;
    }
}

/* Returns a pointer to the cache_entry assigned to SECTOR, if present.
   Otherwise returns NULL. Checks amongst cache_entries cointaing closed
   sectors is CLOSED is true. */
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

  return hash_entry (e, struct cache_entry, elem);
}


/* Chooses a cache_entry for eviction. Takes one from a closed sector 
   if available, otherwise chooses a one using the clock algorithim. Acquires
   a write lock to the cache_entry and returns it. */
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
      ce = hash_entry (e, struct cache_entry, elem);
      rw_lock_acquire_w (&ce->rw_lock);
      return ce;
    } 
  
  while (true)
    {
      cur_index++;
      cur_index %= NUM_CACHE_SECTORS;
      ce = &cache_entries[cur_index];
  
      if (!rw_lock_held_by_current_thread_w (&ce->rw_lock) &&
          rw_lock_try_acquire_w (&ce->rw_lock))
        {
          /* Only necessary to check that the thread is safe to evict. */
          if (ce->accessed > 0)
            {
              ce->accessed--;
              rw_lock_release_w (&ce->rw_lock);
            }
          else
            break;
        }
    }
  
  return ce;
}

/* Searches for SECTOR in the cache. If not present, and cache_entry is 
   assigned, evicting and writing back an entry if necessary. Data is 
   read from the disk to the cache_entry, and a pointer to the entry 
   is returned. */
static struct cache_entry *
cache_get_entry (unsigned sector)
{
  struct cache_entry *ce;

  /* Check: Sector is locked by the current thread. */
  struct thread *t = thread_current ();
  if (t->locked_ce != NULL)
    {
      ce = t->locked_ce;
      ASSERT (ce->sector == sector);
      return t->locked_ce;
    }
  

  /* Check: Sector is already present in the cache, and not closed. */ 
  lock_acquire (&cache_lock);
  ce = cache_lookup (sector, false);
  if (ce != NULL)
    {
      rw_lock_acquire_r (&ce->rw_lock);
      lock_release (&cache_lock);
      return ce;
    }

  /* Check: Sector is present in the cache, but closed. */
  ce = cache_lookup (sector, true);
  if (ce != NULL)
    {
      hash_delete (&cache_closed_hash, &ce->elem);
      hash_insert (&cache_hash, &ce->elem);
      rw_lock_acquire_r (&ce->rw_lock);
      lock_release (&cache_lock);
      return ce;
    }

  /* Sector must be read from the disk. */
  unsigned old_sector;
  bool write_back = false;
  /* Write lock is acquired in cache_evict (). */
  ce = cache_evict ();
  if (ce->dirty)
  {
    old_sector = ce->sector;
    write_back = true;
  }
  /* Must rehash using the new sector. */
  hash_delete (&cache_hash, &ce->elem);
  ce->sector = sector;
  ce->dirty = false;
  hash_insert (&cache_hash, &ce->elem);
  lock_acquire (&evict_lock);
  lock_release (&cache_lock);
  if (write_back)
    block_write (fs_device, old_sector, &ce->data);
  block_read (fs_device, sector, &ce->data); 
  lock_release (&evict_lock);
  rw_lock_demote (&ce->rw_lock);
  return ce;
}

/* Waits for prefetch requests to be queued, then fetches the requested
   sectors before waiting again. Should be run in a high priority thread
   to ensure executation before CACHE_SECTOR_PREFETCH_ASYNC's caller
   attempts to fetch itself. A locked sector should not be fetched. */
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
      rw_lock_release_r (&ce->rw_lock);
    }
}

/* Writes back all dirty cache_entries to the disk and marks them
   clean. */
static void
cache_flush (void)
{
  struct list_elem *e;
  for (e = list_begin (&list_ext); e != list_end (&list_ext); 
       e = list_next (e))
    {
      struct cache_entry_ext *ce = list_entry (e, struct cache_entry_ext, 
                                               ext_elem);
      if (ce->dirty)
        {
          for (unsigned i = 0; i < ce->num_sectors; i++)
            block_write (fs_device, ce->sector + i, 
                         ce->data + i * BLOCK_SECTOR_SIZE);
        }
    }

  for (unsigned i = 0; i < NUM_CACHE_SECTORS; i++)
    {
      struct cache_entry *ce = &cache_entries[i];
      /* Read lock is sufficient to ensure an eviction is not
         attempted. This prevents reading, writing, and locked sectors
         from stalling the loop. */
      rw_lock_acquire_r (&ce->rw_lock);
      if (ce->dirty)
        {
          block_write (fs_device, ce->sector, &ce->data);
          ce->dirty = false;
        }
      rw_lock_release_r (&ce->rw_lock);
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

/* Helper function that handles closing and removing of sectors. */
static void
cache_sector_cr (unsigned sector, bool deleted)
{
  lock_acquire (&cache_lock);
  struct cache_entry *ce = cache_lookup (sector, false);
  if (ce != NULL)
    {
      hash_delete (&cache_hash, &ce->elem);
      hash_insert (&cache_closed_hash, &ce->elem);
      if (deleted)
        ce->dirty = false;
    }
  lock_release (&cache_lock);
}

/* Hashes cache_entries by the sector number it stores. */
static unsigned 
cache_hash_func (const struct hash_elem *e, void *aux UNUSED)
{
  struct cache_entry *ce = hash_entry (e, struct cache_entry, elem);
  return hash_int ((int) ce->sector);
}

/* Comparator for CACHE_HASH. */
static bool 
cache_less_func (const struct hash_elem *a,
                 const struct hash_elem *b,
                 void *aux UNUSED)
{
  struct cache_entry *cea = hash_entry (a, struct cache_entry, elem);
  struct cache_entry *ceb = hash_entry (b, struct cache_entry, elem);
  return cea->sector < ceb->sector;
}
