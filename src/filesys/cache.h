#include <stdbool.h>
void cache_init (void);
void cache_destroy (void);
void cache_sector_read (unsigned, void *, unsigned, unsigned, unsigned);
void cache_sector_write (unsigned, const void *, unsigned, unsigned, unsigned);
void cache_sector_lock (unsigned);
void cache_sector_unlock (unsigned);
void cache_sector_add (unsigned, unsigned);
void cache_sector_fetch_async (unsigned);
void cache_sector_close (unsigned);
void cache_sector_remove (unsigned);
bool cache_sector_read_external (unsigned, void *, unsigned);
void cache_sector_free_external (unsigned);
void cache_sector_dirty_external (unsigned);
