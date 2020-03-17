#include "filesys/directory.h"
#include <stdio.h>
#include <string.h>
#include <list.h>
#include "filesys/filesys.h"
#include "filesys/inode.h"
#include "threads/malloc.h"
#include "threads/thread.h"

static char *current_str = ".";
static char *parent_str = "..";

/* A directory. */
struct dir 
  {
    struct inode *inode;                /* Backing store. */
    off_t pos;                          /* Current position. */
  };

/* A single directory entry. */
struct dir_entry 
  {
    unsigned inumber;        /* Sector number of header. */
    char name[NAME_MAX + 1];            /* Null terminated file name. */
    bool in_use;                        /* In use or free? */
  };

/* Creates a directory with space for ENTRY_CNT entries in the
   given SECTOR.  Returns true if successful, false on failure. */
bool
dir_create (block_sector_t sector, block_sector_t parent)
{
  bool success =  inode_create (sector, 2 * sizeof (struct dir_entry), true);
  if (success == false) 
    return false; 
  
  struct inode *inode = inode_open (sector);
  if (inode == NULL)
    return false;
  struct dir *dir = dir_open (inode);
  if (dir == NULL)
    return false;
  
  success = dir_add (dir, ".", sector) && 
            dir_add (dir, "..", parent);
  dir_close (dir);
  return success;
}

/* Opens and returns the directory for the given INODE, of which
   it takes ownership.  Returns a null pointer on failure. */
struct dir *
dir_open (struct inode *inode) 
{
  struct dir *dir = calloc (1, sizeof *dir);
  if (inode != NULL && dir != NULL)
    {
      dir->inode = inode;
      dir->pos = 0;
      return dir;
    }
  else
    {
      inode_close (inode);
      free (dir);
      return NULL; 
    }
}

/* Opens the root directory and returns a directory for it.
   Return true if successful, false on failure. */
struct dir *
dir_open_root (void)
{
  return dir_open (inode_open (ROOT_DIR_INUMBER));
}

/* Opens and returns a new directory for the same inode as DIR.
   Returns a null pointer on failure. */
struct dir *
dir_reopen (struct dir *dir) 
{
  return dir_open (inode_reopen (dir->inode));
}

/* Destroys DIR and frees associated resources. */
void
dir_close (struct dir *dir) 
{
  if (dir != NULL)
    {
      inode_close (dir->inode);
      free (dir);
    }
}

/* Returns the inode encapsulated by DIR. */
struct inode *
dir_get_inode (struct dir *dir) 
{
  return dir->inode;
}

/* Searches DIR for a file with the given NAME.
   If successful, returns true, sets *EP to the directory entry
   if EP is non-null, and sets *OFSP to the byte offset of the
   directory entry if OFSP is non-null.
   otherwise, returns false and ignores EP and OFSP. */
static bool
lookup (const struct dir *dir, const char *name,
        struct dir_entry *ep, off_t *ofsp) 
{
  struct dir_entry e;
  size_t ofs;
  
  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  bool success = false;  
  bool prev = inode_lock_dir (dir->inode);
  for (ofs = 0; inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
       ofs += sizeof e) 
    {
    if (e.in_use && !strcmp (name, e.name)) 
      {
        if (ep != NULL)
          *ep = e;
        if (ofsp != NULL)
          *ofsp = ofs;
        success = true;
        break;
      }
    }

  inode_set_lock_dir (dir->inode, prev);
  return success;
}


/* Deletes a chain of the first file found in dir. Assumes that all of these
   files are empty directories. */
static bool
dir_is_empty (struct dir *dir)
{
  struct dir_entry e;
  size_t ofs;
  ASSERT (dir != NULL);
  bool success = true;
  bool prev = inode_lock_dir (dir->inode);
  for (ofs = 0; inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
       ofs += sizeof e) 
    {
      if (e.in_use && strcmp (e.name, ".") && 
          strcmp (e.name, ".."))
        success = false;
    }
  inode_set_lock_dir (dir->inode, prev);
  return success;
}

/* Retruns true if the inode tied to a directory has been
   removed. */
static bool
dir_is_removed (struct dir *dir)
{
  if (inode_is_removed)
  return inode_is_removed (dir->inode);
}

/* Searches DIR for a file with the given NAME
   and returns true if one exists, false otherwise.
   On success, sets *INODE to an inode for the file, otherwise to
   a null pointer.  The caller must close *INODE. */
bool
dir_lookup (const struct dir *dir, const char *name,
            struct inode **inode) 
{
  struct dir_entry e;
  bool prev = inode_lock_dir (dir->inode);
  if (dir_is_removed (dir))
    {
      inode_set_lock_dir (dir->inode, prev);
      return false;
    }
  ASSERT (dir != NULL);
  ASSERT (name != NULL);
  if (lookup (dir, name, &e, NULL))
    *inode = inode_open (e.inumber);
  else
    *inode = NULL;
  inode_set_lock_dir (dir->inode, prev);
  return *inode != NULL;
}

/* Adds a file named NAME to DIR, which must not already contain a
   file by that name.  The file's inode is in sector
   INODE_SECTOR.
   Returns true if successful, false on failure.
   Fails if NAME is invalid (i.e. too long) or a disk or memory
   error occurs. */
bool
dir_add (struct dir *dir, const char *name, inumber_t inumber)
{
  struct dir_entry e;
  off_t ofs;
  bool success = false;
  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  /* Check NAME for validity. */
  if (*name == '\0' || strlen (name) > NAME_MAX)
    return false;

  bool prev = inode_lock_dir (dir->inode);
  if (dir_is_removed (dir))
    {
      inode_set_lock_dir (dir->inode, prev);
      return false;
    }

  /* Check that NAME is not in use. */
  if (lookup (dir, name, NULL, NULL))
  {
    goto done;
}
  /* Set OFS to offset of free slot.
     If there are no free slots, then it will be set to the
     current end-of-file.
     
     inode_read_at() will only return a short read at end of file.
     Otherwise, we'd need to verify that we didn't get a short
     read due to something intermittent such as low memory. */
  for (ofs = 0; inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
       ofs += sizeof e) 
    if (!e.in_use)
      break;

  /* Write slot. */
  e.in_use = true;
  strlcpy (e.name, name, sizeof e.name);
  e.inumber = inumber;
  success = inode_write_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
  struct dir_entry test;
  inode_read_at (dir->inode, &test, sizeof test, ofs);
  inode_set_lock_dir (dir->inode, prev);
 done:
  return success;
}

/* Removes any entry for NAME in DIR, except for non-empty
   directories. Returns true if successful, false on failure,
   which occurs only if there is no file with the given NAME. */
bool
dir_remove (struct dir *dir, const char *name) 
{
  struct dir_entry e;
  struct inode *inode = NULL;
  bool success = false;
  off_t ofs;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  /* Find directory entry. */
  if (!lookup (dir, name, &e, &ofs))
    goto done;

  /* Open inode. */
  inode = inode_open (e.inumber);
  if (inode == NULL)
    goto done;
  
  if (inode_is_dir (inode))
    {
      struct dir *rmdir = dir_open (inode);
      if (rmdir == NULL)
        return false;
      if (!dir_is_empty (rmdir))
        {
          dir_close (rmdir);
          return false;
        }
      free (rmdir);
    }
 
  /* Erase directory entry. */
  e.in_use = false;
  if (inode_write_at (dir->inode, &e, sizeof e, ofs) != sizeof e) 
    goto done;

  /* Remove inode. */
  inode_remove (inode);
  success = true;

 done:
  inode_close (inode);
  return success;
}

/* Reads the next directory entry in DIR and stores the name in
   NAME.  Returns true if successful, false if the directory
   contains no more entries. */
bool
dir_readdir (struct dir *dir, char name[NAME_MAX + 1])
{
  struct dir_entry e;
  bool success = false;
  bool prev = inode_lock_dir (dir->inode);
  while (inode_read_at (dir->inode, &e, sizeof e, dir->pos) == sizeof e) 
    {
      dir->pos += sizeof e;
      if (e.in_use && strcmp (e.name, ".") && strcmp (e.name, ".."))
        {
          strlcpy (name, e.name, NAME_MAX + 1);
          success = true;
          break;
        } 
    }
  inode_set_lock_dir (dir->inode, prev);
  return success;
}

/* Returns a pointer to the stat of a filename, or
   to a slash if a single slash is supplied. Returns NULL
   otherwise. */
char *
dir_file (const char *name)
{
  char *begin = strrchr (name, '/');
  
  /* File is in current directory. Return name. */
  if (begin == NULL)
    return (char *) name;

  begin += 1;
  int len = strlen (begin);
  if (len == 0 || len > NAME_MAX) 
    return NULL;

  return begin;
}

/* Finds the directory NAME */ 
struct dir *
dir_fetch (char *name, char **file) 
{
  unsigned len;
  if (name == NULL || (len = strlen (name)) == 0)
    return NULL;
  
  struct thread *t = thread_current ();
  struct dir *cur_dir;
  /* Absolute path */  
  if (name[0] == '/') {
    cur_dir = dir_open_root ();
    if (len == 1)
     {
      if (cur_dir != NULL && file != NULL)
        *file = current_str;
      return cur_dir;
     }
  } else {
  /* Relative path */
    if (t->cwd == NULL)
      t->cwd = dir_open_root ();
    cur_dir = dir_reopen (t->cwd);
  }
  
  if (cur_dir == NULL)
    return NULL;

  char *path_end;
  if (file != NULL)
    {
      path_end = dir_file (name);
      if (path_end == NULL)
        return NULL; 
    }
  else
    path_end = name + strlen (name);

  len = path_end - name;
  char *path = malloc (len + 1);
  if (path == NULL)
    return NULL;
  strlcpy (path, name, len + 1);
  
  char *saved; 
  char *next;
  for (next = strtok_r (path, "/", &saved); next != NULL;
       next = strtok_r (NULL, "/", &saved))
  {
    struct inode *inode;
    if (dir_lookup (cur_dir, next, &inode))
      {
        /* next directory is found in cur_dir */
        dir_close (cur_dir);
        cur_dir = dir_open (inode);
        if (cur_dir == NULL)
          break;
      }
    else
      {
        dir_close (cur_dir);
        cur_dir = NULL;
        break;
      }
  
  }
  free (path);
  if (cur_dir != NULL && file != NULL)
    *file = path_end;
  return cur_dir; 
}

  
