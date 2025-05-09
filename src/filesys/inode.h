#ifndef FILESYS_INODE_H
#define FILESYS_INODE_H

#include <stdbool.h>
#include "filesys/off_t.h"
#include "devices/block.h"

typedef unsigned inumber_t;
struct bitmap;

void inode_init (void);
bool inode_assign_inumber (inumber_t *);
void inode_release_inumber (inumber_t);
bool inode_create (inumber_t, off_t, bool is_dir);
struct inode *inode_open (inumber_t);
struct inode *inode_reopen (struct inode *);
inumber_t inode_get_inumber (const struct inode *);
void inode_close (struct inode *);
void inode_remove (struct inode *);
bool inode_is_removed (struct inode *);
off_t inode_read_at (struct inode *, void *, off_t size, off_t offset);
off_t inode_write_at (struct inode *, const void *, off_t size, off_t offset);
void inode_deny_write (struct inode *);
void inode_allow_write (struct inode *);
off_t inode_length (const struct inode *);
bool inode_is_dir (const struct inode *);
bool inode_lock_dir (struct inode *);
void inode_set_lock_dir (struct inode *, bool);
#endif /* filesys/inode.h */
