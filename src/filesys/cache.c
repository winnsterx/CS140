#include "filesys/cache.h"
#include <debug.h>
#include <string.h>
#include "devices/block.h"
#include "filesys/filesys.h"
#include "lib/kernel/bitmap.h"
#include "lib/kernel/hash.h"
#include "threads/synch.h"
// WAIT, WE DONT REALLY NEED AN EVICT LIST. WE'RE LITERALLY USING AN ARRAY
// KEEP AROUND FOR NOW IN CASE APPROACH GETS ROASTED
#define NUM_CACHE_SECTORS 64

/* Is it better to preallocate all pages? Ask, want 100% */
struct list cache_free_list;
struct list cache_evict_list;
struct hash cache_hash;
struct cache_entry
  {
    unsigned sector;
    struct hash_elem hash_elem;
    struct list_elem list_elem;
    struct lock lock;
    bool accessed;
    bool dirty;
    bool is_meta;
    uint8_t data[BLOCK_SECTOR_SIZE];
  };

/* Used for hash searches without allocating an entire
   CACHE_ENTRY. Must have same offset between SECTOR and
   ELEM as CACHE_ENTRY. */
struct cache_entry_stub
  {
    unsigned sector;
    struct hash_elem elem;
  };

static struct cache_entry cache_entries[NUM_CACHE_SECTORS];
static struct lock cache_lock;
static struct list_elem *cur_cache_elem;

static void cache_flush (void);
static struct cache_entry *cache_get_entry (unsigned);
static unsigned cache_hash_func (const struct hash_elem *, void *);
static bool cache_less_func (const struct hash_elem *, 
                             const struct hash_elem *, void *) ;

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
  list_init (&cache_evict_list);
  cur_cache_elem = list_end (&cache_evict_list);
  for (unsigned i = 0; i < NUM_CACHE_SECTORS; i++)
   {
    struct cache_entry *ce = &cache_entries[i];
    lock_init (&ce->lock);
    list_push_back (&cache_free_list, &ce->list_elem);
   }

 // thread_start (cache_write_back_loop ());
}

/* Writes dirty sectors and frees all resources. */
void 
cache_destroy (void)
{
  cache_flush ();
  hash_destroy (&cache_hash, NULL);
} 

void 
cache_sector_read (unsigned sector, void *buffer, unsigned size, unsigned offset)
{
  ASSERT (offset + size <= BLOCK_SECTOR_SIZE);
  struct cache_entry *ce = cache_get_entry (sector);
  ce->accessed = true;
  memcpy (buffer, &ce->data[offset], size);
  lock_release (&ce->lock);
}

void 
cache_sector_write (unsigned sector, void *buffer, unsigned size, unsigned offset)
{
  ASSERT (offset + size <= BLOCK_SECTOR_SIZE);
  struct cache_entry *ce = cache_get_entry (sector);
  ce->accessed = true;
  ce->dirty = true;
  memcpy (&ce->data[offset], buffer, size);
  lock_release (&ce->lock);
}

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

static struct cache_entry *
cache_evict (void)
{
  ASSERT (lock_held_by_current_thread (&cache_lock));
  if (cur_cache_elem == list_end (&cache_evict_list))
    cur_cache_elem = list_begin (&cache_evict_list);
  else
    cur_cache_elem = list_next (cur_cache_elem);

  struct cache_entry *ce;
  while (true)
    {
      if (cur_cache_elem == list_end (&cache_evict_list))
        cur_cache_elem = list_begin (&cache_evict_list);

      ce = list_entry (cur_cache_elem, struct cache_entry, list_elem);
      /* If acquisition fails, the entry is currently being accessed.
         lock is released  when read/write is complete. */
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
      cur_cache_elem = list_next (cur_cache_elem);
    }

  hash_delete (&cache_hash, &ce->hash_elem);

  return ce;
}

/* THIS SHOULD BE IN A HIGH PRORITY LOOPING THREAD, DONATION */
// PROB SAFE TO UNLOCK CACHE LOCK? CHECK FILESYS DONE
static void
cache_flush (void)
{
  struct list_elem *e;
  lock_acquire (&cache_lock);
  for (e = list_begin (&cache_evict_list); 
       e != list_end (&cache_evict_list); 
       e = list_next (e))
    {
      struct cache_entry *ce = list_entry (e, struct cache_entry, list_elem);
      lock_acquire (&ce->lock);
      lock_release (&cache_lock);
      if (ce->dirty)
        {
          block_write (fs_device, ce->sector, &ce->data);
          ce->dirty = false;
        }
      lock_acquire (&cache_lock);
      lock_release (&ce->lock);
    }
  lock_release (&cache_lock);
}

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
      /* Add just before current place in evict list for
         fairness. */
      list_insert (cur_cache_elem, &ce->list_elem);
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

static unsigned 
cache_hash_func (const struct hash_elem *e, void *aux UNUSED)
{
  struct cache_entry *ce = hash_entry (e, struct cache_entry, hash_elem);
  return hash_int ((int) ce->sector);
}

static bool 
cache_less_func (const struct hash_elem *a,
                 const struct hash_elem *b,
                 void *aux UNUSED)
{
  struct cache_entry *cea = hash_entry (a, struct cache_entry, hash_elem);
  struct cache_entry *ceb = hash_entry (b, struct cache_entry, hash_elem);
  return cea->sector < ceb->sector;
}
