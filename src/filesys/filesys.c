#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/cache.h"
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"

/* Partition that contains the file system. */
struct block *fs_device;

static void do_format (void);

/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void
filesys_init (bool format) 
{
  fs_device = block_get_role (BLOCK_FILESYS);
  if (fs_device == NULL)
    PANIC ("No file system device found, can't initialize file system.");

  inode_init ();
  free_map_init ();
  cache_init ();

  if (format) 
    do_format ();

  free_map_open ();
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void
filesys_done (void) 
{
  free_map_close ();
  cache_destroy ();
}

/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
bool
filesys_create (const char *name, off_t initial_size) 
{
  /* 0 is reserved for the root directory. */
  inumber_t inumber = 0;
  char *file_name;
  struct dir *dir = dir_fetch (name, &file_name);
  bool success = (dir != NULL
                  && inode_assign_inumber (&inumber)
                  && inode_create (inumber, initial_size, false)
                  && dir_add (dir, file_name, inumber));
  if (!success && inumber != 0) 
    inode_release_inumber (inumber);
  dir_close (dir);
  return success;
}

/* Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
struct file *
filesys_open (const char *name)
{
  char *file_name;
  struct dir *dir = dir_fetch (name, &file_name);
  if (dir == NULL)
    return NULL;
  struct inode *inode;
  bool success = dir_lookup (dir, file_name, &inode);
  dir_close (dir);

  return success ? file_open (inode) : NULL;
}

/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool
filesys_remove (const char *name) 
{
  char *file_name;
  struct dir *dir = dir_fetch (name, &file_name);

  bool success = dir != NULL && 
                 dir_remove (dir, file_name);
  dir_close (dir); 
  return success;
}

/* Formats the file system. */
static void
do_format (void)
{
  /* Clear the inode table. */
  
  for (unsigned i = 0; i < INODE_TABLE_SECTORS; i++)
    cache_sector_add (i, PRI_INODE);    
  
  free_map_create ();
  if (!dir_create (ROOT_DIR_INUMBER, ROOT_DIR_INUMBER))
    PANIC ("root directory creation failed");
  free_map_close ();
}
