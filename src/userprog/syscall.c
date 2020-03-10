#include "userprog/syscall.h"
#include <limits.h>
#include <stdio.h>
#include <syscall-nr.h>
#include "filesys/file.h"
#include "devices/input.h"
#include "devices/shutdown.h"
#include "filesys/filesys.h"
#include "lib/kernel/console.h"
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/process.h"

static void syscall_handler (struct intr_frame *);
static void halt (void);
static void exit (int status);
static tid_t exec (const char *name);
static int wait (tid_t pid);
static bool create (const char *name, unsigned initial_size);
static bool remove (const char *name);
static int open (const char *name);
static int filesize (int fd);
static int read (int fd, void *buffer, unsigned length);
static int write (int fd, const void *buffer, unsigned length);
static void seek (int fd, unsigned position);
static unsigned tell (int fd);
static void close (int fd);

static bool chdir (const char *dir);
static bool mkdir (const char *dir);
static bool readdir (int fd, char *name);
static bool isdir (int fd);
static int inumber (int fd);

static int get_user_byte (const uint8_t *);
static unsigned overflow_adjusted_size (const uint8_t *, unsigned);
static uint8_t *first_invalid_uaddr (const uint8_t *, unsigned);
static bool validate_range (const uint8_t *, unsigned);
static bool validate_name (const char *);
static struct fd_struct *elem_to_fd (const struct list_elem *);
static bool fd_gap (const struct list_elem *, void *);
static bool fd_geq (const struct list_elem *, void *);
static struct fd_struct *find_fd_struct (int);
static void free_fd_struct_file (struct fd_struct *);

struct fd_struct
  {
    int fd;
    struct file *file;
    struct list_elem elem;
  };

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

/* Free all of a thread's fd structs and their files. */
void
close_all_files (void)
{
  struct thread *cur = thread_current ();
  struct list_elem *e;
  for (e = list_begin (&cur->fd_list); e != list_end (&cur->fd_list);)
    {
      struct fd_struct *fd_struct = elem_to_fd (e);
      e = list_next (e);
      free_fd_struct_file (fd_struct);
      free (fd_struct);
    }
}

/* Handles incoming syscalls and dispatches the correct function. */
static void
syscall_handler (struct intr_frame *f UNUSED) 
{
  if (!validate_range (f->esp, 4 * sizeof (int)))
    thread_exit ();
 
  enum syscall_no syscall_no = *(enum syscall_no *) f->esp;
  void *arg1 = f->esp + sizeof (int);
  void *arg2 = f->esp + 2 * sizeof (int);
  void *arg3 = f->esp + 3 * sizeof (int);

  switch (syscall_no)
    {
      case SYS_HALT:
        halt ();
        break;
      case SYS_EXIT:
        exit (*(int *) arg1);
        break;
      case SYS_EXEC:
        f->eax = exec (*(const char **) arg1);
        break;
      case SYS_WAIT:
        f->eax = wait (*(tid_t *) arg1);
        break;
      case SYS_CREATE:
        f->eax = create (*(const char **) arg1, *(unsigned *) arg2);
        break;
      case SYS_REMOVE:
        f->eax = remove (*(const char **) arg1);
        break;
      case SYS_OPEN:
        f->eax = open (*(const char **) arg1);
        break;
      case SYS_FILESIZE:
        f->eax = filesize (*(int *) arg1);
        break;
      case SYS_READ:
        f->eax = read (*(int *) arg1, *(void **) arg2,
                       *(unsigned *) arg3);
        break;
      case SYS_WRITE:
        f->eax = write (*(int *) arg1, *(const void **) arg2,
                        *(unsigned *) arg3);
        break;
      case SYS_SEEK:
        seek (*(int *) arg1, *(unsigned *) arg2);
        break;
      case SYS_TELL:
        f->eax = tell (*(int *) arg1);
        break;
      case SYS_CLOSE:
        close (*(int *) arg1);
        break;
      default:
        thread_exit ();
    }
}

NO_RETURN static void
halt (void)
{
  shutdown_power_off ();
}

NO_RETURN static void
exit (int status)
{
  struct thread *cur = thread_current ();  
  cur->proc_state->exit_status = status;
  thread_exit ();
}

static tid_t
exec (const char *name)
{
  if (!validate_name (name))
    thread_exit ();

  return process_execute (name);
}

static int
wait (tid_t pid)
{
  return process_wait (pid);
}

static bool
create (const char *name, unsigned initial_size)
{
  if (!validate_name (name))
    thread_exit ();

  bool success;
  lock_acquire (&thread_filesys_lock);
  success = filesys_create (name, initial_size);
  lock_release (&thread_filesys_lock);

  return success;
}

static bool
remove (const char *name)
{
  if (!validate_name (name))
    thread_exit ();

  bool success;
  lock_acquire (&thread_filesys_lock);
  success = filesys_remove (name);
  lock_release (&thread_filesys_lock);
 
  return success;
}

static int
open (const char *name)
{
  if (!validate_name (name))
    thread_exit ();

  lock_acquire (&thread_filesys_lock);
  struct file *file = filesys_open (name);
  lock_release (&thread_filesys_lock);
  if (file == NULL)
    return -1;

  struct list *fd_list = &thread_current ()->fd_list;
  void *head = list_head (fd_list);
  struct list_elem *e = list_search_first (fd_list, fd_gap, head);
  struct fd_struct *fd_struct = malloc (sizeof (struct fd_struct));
  if (fd_struct == NULL)
    {
      file_close (file);
      return -1;
    }

  int fd;
  if (list_prev (e) == head)
    fd = 2;
  else
    fd = elem_to_fd (list_prev (e))->fd + 1;

  fd_struct->fd = fd;
  fd_struct->file = file;
  list_insert (e, &fd_struct->elem);

  return fd;
}

static int
filesize (int fd)
{
  struct fd_struct *fd_struct = find_fd_struct (fd);
  if (fd_struct == NULL)
    return -1;
  
  lock_acquire (&thread_filesys_lock);
  int length = file_length (fd_struct->file);
  lock_release (&thread_filesys_lock);

  return length;
}

static int
read (int fd, void *buffer, unsigned length)
{
  if (!validate_range (buffer, length))
    thread_exit ();

  if (fd == STDOUT_FILENO)
    return -1;

  if (fd == STDIN_FILENO)
    {
      unsigned i = 0;
      for (; i < length; i++)
        {
          ((uint8_t *) buffer)[i] = input_getc ();
        }
      
      return i;  
    }
  
  struct fd_struct *fd_struct = find_fd_struct (fd);
  if (fd_struct == NULL)
    return -1;
  
  int result;
  lock_acquire (&thread_filesys_lock);
  result = file_read (fd_struct->file, buffer, length);
  lock_release (&thread_filesys_lock);
  
  return result;
}

static int
write (int fd, const void *buffer, unsigned length)
{
  if (!validate_range (buffer, length))
    thread_exit ();

  if (fd == STDIN_FILENO)
    return -1;

  if (fd == STDOUT_FILENO)
   {
     putbuf (buffer, length);
     return length;
   }

  struct fd_struct *fd_struct = find_fd_struct (fd);
  if (fd_struct == NULL)
    return -1;

  int result;
  lock_acquire (&thread_filesys_lock);
  result = file_write (fd_struct->file, buffer, length);
  lock_release (&thread_filesys_lock);

  return result;
}

static void
seek (int fd, unsigned position)
{
  struct fd_struct *fd_struct = find_fd_struct (fd);
  if (fd_struct == NULL)
    return;

  lock_acquire (&thread_filesys_lock);
  file_seek (fd_struct->file, position);
  lock_release (&thread_filesys_lock);
}

static unsigned
tell (int fd)
{
  struct fd_struct *fd_struct = find_fd_struct (fd);
  if (fd_struct == NULL)
    thread_exit ();
  
  unsigned pos;
  lock_acquire (&thread_filesys_lock);
  pos = file_tell (fd_struct->file);
  lock_release (&thread_filesys_lock);

  return pos;
}

static void
close (int fd)
{
  struct fd_struct *fd_struct = find_fd_struct (fd);
  if (fd_struct == NULL)
    thread_exit ();

  free_fd_struct_file (fd_struct);
  free (fd_struct);
}

/* Dereferences a byte at addres UADDR. Returns -1 if the address is
   unmapped or invalid, otherwise returns the byte held by the address.
   This function assumes UADDR < PHYS_BASE. */
static int
get_user_byte (const uint8_t *uaddr)
{
  int result;
  asm ("movl $1f, %0; movzbl %1, %0; 1:"
       : "=&a" (result) : "m" (*uaddr));

  return result;
}

/* Checks whether adding SIZE to UADDR would overflow UADDR. If not, size
   is returned. Otherwize, the largest possible size without overflow
   is returned. */
static unsigned overflow_adjusted_size (const uint8_t *uaddr, unsigned size)
{
  if (uaddr + size < uaddr)
    return UINT_MAX - (unsigned) uaddr;

  else
    return size;
}

/* In a range of user addresses starting at UADDR, this returns the first
   invalid user address, or the first address beyond the specified range, 
   whichever comes first. Returns UADDR if the UADDR is invalid. */
static uint8_t *
first_invalid_uaddr (const uint8_t *uaddr, unsigned size)
{
  if (uaddr == NULL || size == 0)
    return (uint8_t *) uaddr;
 
  size = overflow_adjusted_size (uaddr, size);

  uint8_t *pg_ptr = pg_round_down (uaddr);
  uint8_t *first_pg_ptr = pg_ptr;
  while (pg_ptr < uaddr + size)
    {
      if (is_kernel_vaddr (pg_ptr) || get_user_byte (pg_ptr) == -1)
        {
          if (pg_ptr == first_pg_ptr)
            return (uint8_t *) uaddr;
          
          break;
        }
      pg_ptr += PGSIZE;
    }

  return pg_ptr;
}

/* Returns true if and only if UADDR and SIZE descripe an non-empty
   range, where every user address contained is valid */
static bool
validate_range (const uint8_t *uaddr, unsigned size)
{
  if (size != overflow_adjusted_size (uaddr, size))
    return false;
 
  uint8_t *ptr = first_invalid_uaddr (uaddr, size);

  return ptr >= uaddr + size;
}

/* Ensures a name is less than a page long, and lives in 
   valid user memory */
static bool 
validate_name (const char *name)
{
  char* highest = (char*) first_invalid_uaddr ((uint8_t *) name, PGSIZE);
  if (highest == name)
    return false;
  
  unsigned max = highest - name;
  max = max > PGSIZE ? PGSIZE : max;

  for (unsigned i = 0; i < max; i++)
    {
      if (name[i] == '\0')
        return true;
    }
  
  return false;
}

/* Converts a list element pointer to its corresponding
   struct pointer */
static inline struct fd_struct *
elem_to_fd (const struct list_elem *e)
{
   return list_entry (e, struct fd_struct, elem);
}

/* Returns true if there is a gap between the current
   and previous fds in the list */
static bool 
fd_gap (const struct list_elem *cur, void *aux)
{
  struct list_elem *fd_list_head = (struct list_elem *) aux;
  struct list_elem *prev = list_prev ((struct list_elem *) cur);
  if (prev == fd_list_head)
    return elem_to_fd (cur)->fd > 2;

  return (elem_to_fd (cur)->fd > elem_to_fd (prev)->fd + 1);
}

/* Returns if the passed elements fd is greater than
   or equal to the fd passed into aux. */
static bool 
fd_geq (const struct list_elem *cur, void *aux)
{
  int fd = (int) aux;
  return elem_to_fd (cur)->fd >= fd;
}

/* Searches for struct of corresponding fd. Returns NULL if it is not
   present */
static struct fd_struct *
find_fd_struct (int fd)
{  
  struct thread *cur = thread_current ();
  struct list_elem *e = list_search_first (&cur->fd_list,
                                           fd_geq, (void *) fd);

  if (e ==list_end (&cur->fd_list))
  if (e == list_end (&cur->fd_list) || elem_to_fd (e)->fd != fd)
    return NULL;

  return elem_to_fd (e);
}

/* Frees the file in an fd struct and removes the struct from 
   its list. Does not free the struct. */
static void
free_fd_struct_file (struct fd_struct *fd_struct)
{
  list_remove (&fd_struct->elem);
  lock_acquire (&thread_filesys_lock);
  file_close (fd_struct->file);
  lock_release (&thread_filesys_lock);
}

