#include "userprog/syscall.h"
#include <limits.h>
#include <stdio.h>
#include <syscall-nr.h>
#include "lib/kernel/console.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"

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

static void
syscall_handler (struct intr_frame *f UNUSED) 
{
  /* Assuming a proper syscall, there should be at least 4
     words on the caller stack */
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
}

static bool
create (const char *name, unsigned initial_size)
{
}

static bool
remove (const char *name)
{
}

static int
open (const char *name)
{
}

static int
filesize (int fd)
{
}

static int
read (int fd, void *buffer, unsigned length)
{
}

static int
write (int fd, const void *buffer, unsigned length)
{
  if (fd == STDIN_FILENO || !validate_range (buffer, length))
    return -1;

 /* INDENT STYLE? */ 
 if (fd == STDOUT_FILENO)
    {
      putbuf (buffer, length);
      return length;
    }
  else
    {
    }
}

static void
seek (int fd, unsigned position)
{
}

static unsigned
tell (int fd)
{
}

static void
close (int fd)
{
}
