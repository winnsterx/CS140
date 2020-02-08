#include "userprog/syscall.h"
#include <limits.h>
#include <stdio.h>
#include <syscall-nr.h>
#include "lib/kernel/console.h"
#include "threads/interrupt.h"
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

static int
get_user_byte (const uint8_t *uaddr)
{
  int result;
  asm ("movl $1f, %0; movzbl %1, %0; 1:"
       : "=&a" (result) : "m" (*uaddr));

  return result;
}

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
static const uint8_t *
first_invalid_uaddr (const uint8_t *uaddr, unsigned size)
{
  /* Check size = 0 case */
  if (uaddr == NULL || size == 0)
    return uaddr;
  
  size = overflow_adjusted_size (uaddr, size);

  uint8_t *pg_ptr = pg_round_down (uaddr);
  uint8_t *first_pg_ptr = pg_ptr;
  while (pg_ptr < uaddr + size)
    {
      if (is_kernel_vaddr (pg_ptr) || get_user_byte (pg_ptr) == -1)
        {
          if (pg_ptr == first_pg_ptr)
            return uaddr;
          
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

static bool 
validate_name (const char *name)
{
  char* highest = first_invalid_uaddr (name, PGSIZE);
  if (highest == name)
    return false;
  
  unsigned max = highest - name;
  max = max > PGSIZE ? PGSIZE : max;

  for (int i = 0; i < max; i++)
    {
      if (name[i] == '\0')
        return true;
    }
  
  return false;
}

static struct fd_struct *
elem_to_fd (struct list_elem *e)
{
   return list_entry (e, struct fd_struct, elem);
}

static bool 
fd_gap (const struct list_elem *cur, void *aux)
{
  struct list_elem *fd_list_head = (struct list_elem *) aux;
  struct list_elem *prev = list_prev (cur);
  if (prev == fd_list_head)
    return elem_to_fd (cur)->fd > 2;

  return (elem_to_fd (cur)->fd > elem_to_fd (prev)->fd + 1);
}

static bool 
fd_leq (const struct list_elem *cur, void *aux)
{
  int fd = (int) aux;
   return elem_to_fd (cur)->fd >= fd;
}

static void
free_fd_struct (struct fd_struct *fd_struct)
  {
    list_remove (&fd_struct->elem);
    lock_acquire (&thread_filesys_lock);
    file_close (fd_struct->file);
    lock_release (&thread_filesys_lock);
    if (!validate_range (fd_struct, sizeof fd_struct))
      printf ("WHAT THE HELL\n");
    free (fd_struct);
   }

static struct thread_struct *
find_fd_struct (int fd)
{  
  struct thread *cur = thread_current ();
  struct list_elem *e = list_search_first (&cur->fd_list,
                                           fd_leq, (void *) fd);

  if (e ==list_end (&cur->fd_list))
  if (e == list_end (&cur->fd_list) || elem_to_fd (e)->fd != fd)
    return NULL;

  return elem_to_fd (e);
}

static void
syscall_handler (struct intr_frame *f UNUSED) 
{

  // hex_dump (f->esp, f->esp, 100, true);
  /* Assuming a proper syscall, there should be at least 4
     words on the caller stack */

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
        /* Not a valid syscall. */
        thread_exit ();
    }
}

NO_RETURN static void
halt (void)
{
}

NO_RETURN static void
exit (int status)
{
  struct thread *cur = thread_current ();  
  cur->wait->exit_status = status;
  struct list_elem *e;
  for (e = list_begin (&cur->fd_list); e != list_end (&cur->fd_list);
       e = list_next (e))
    {
      free_fd_struct (elem_to_fd (e));
    }  

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
  success = filesys_remove ();
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
   {
    printf ("open failed\n");
    return -1;
  }

  struct list *fd_list = &thread_current ()->fd_list;
  void *head = list_head (fd_list);
  struct list_elem *e = list_search_first (fd_list, fd_gap, head);
  
  // WHAT IF MALLOC FAILS

  struct fd_struct *fd_struct = malloc (sizeof (struct fd_struct));
  int fd;

  if (list_prev (e) == head)
    fd = 2;
  else
    fd = elem_to_fd (list_prev (e))->fd + 1;

  fd_struct->fd = fd;
  fd_struct->file = file;
  printf ("POINTER: %p\n", file);
  list_insert (e, &fd_struct->elem);

  printf ("FUCK\n");
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

  return file_length (fd_struct->file);
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
      int i = 0;
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
  printf ("start reading: %p\n", fd_struct->file);
  result = file_read (fd_struct->file, buffer, length);
  printf ("reading done\n");
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
  file_seek (fd, position);
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
  file_tell (fd_struct->file);
  lock_release (&thread_filesys_lock);

  return pos;
}

static void
close (int fd)
{
  struct fd_struct *fd_struct = find_fd_struct (fd);
  if (fd_struct == NULL)
    thread_exit (); //should we be this harsh?

  free_fd_struct (fd_struct);  
}
