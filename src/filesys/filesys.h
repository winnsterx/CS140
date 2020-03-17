#ifndef FILESYS_FILESYS_H
#define FILESYS_FILESYS_H

#include <stdbool.h>
#include "filesys/off_t.h"

/* Size of the inode table in sectors. */
#define INODE_TABLE_SECTORS 100

/* inumbers of system file inodes. */
//#define FREE_MAP_INUMBER 0
#define ROOT_DIR_INUMBER 0

#define PRI_INODE 3
#define PRI_META 2
#define PRI_NORMAL 1

/* Block device that contains the file system. */
struct block *fs_device;

void filesys_init (bool format);
void filesys_done (void);
bool filesys_create (const char *name, off_t initial_size);
struct file *filesys_open (const char *name);
bool filesys_remove (const char *name);

#endif /* filesys/filesys.h */
