void cache_init (void);
void cache_destroy (void);
void cache_sector_read (unsigned, void *, unsigned, unsigned);
void cache_sector_write (unsigned, void *, unsigned, unsigned);
void cache_sector_fetch (unsigned);
void cache_sector_close (unsigned);
void cache_sector_remove (unsigned);
