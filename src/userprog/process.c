#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/syscall.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"

struct process_args
  {
    char *command;
    struct dir *cwd;
    struct process_state *proc_state;
    struct semaphore process_loaded_sem;
    bool success;
  };

static thread_func start_process NO_RETURN;
static bool load (const char *cmdline, void (**eip) (void), void **esp);

/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
tid_t
process_execute (const char *command) 
{
  char *cmd_copy;
  tid_t tid;

  /* Make a copy of FILE_NAME.
     Otherwise there's a race between the caller and load(). */
  cmd_copy = palloc_get_page (0);
  if (cmd_copy == NULL)
    return TID_ERROR;
  strlcpy (cmd_copy, command, PGSIZE);

  /* Sets thread's name, ensures it is null terminated. 
     Found sizeof trick on stackoverflow. */
  unsigned sizeof_name = sizeof ((struct thread *)0)->name;
  char file_name[sizeof_name];
  strlcpy (file_name, command, sizeof_name);
  char *name_end = strchr (file_name, ' ');
  if (name_end != NULL)
    *name_end = '\0';
  
  struct process_state *proc_state = malloc (sizeof (struct process_state));
  if (proc_state == NULL)
    return TID_ERROR;
  
  sema_init (&proc_state->wait_sem, 0);
  proc_state->exit_status = -1; /* Changed on a proper exit */
  proc_state->parent_alive = true;
  proc_state->child_alive = true;

  /* Get arguments to START_PROCESS */
  struct process_args process_args;
  process_args.command = cmd_copy;
  process_args.proc_state = proc_state;
  process_args.cwd = thread_current ()->cwd;
  sema_init (&process_args.process_loaded_sem, 0);

  /* Create a new thread to execute FILE_NAME. */
  tid = thread_create (file_name, PRI_DEFAULT,
		       start_process, &process_args);

  if (tid == TID_ERROR)
    {
      palloc_free_page (cmd_copy);
      lock_release (&proc_state->status_lock);
      free (proc_state);
      return TID_ERROR;
    }
  
  sema_down (&process_args.process_loaded_sem); 
  if (!process_args.success)
    {
      /* Free child if it has already marked itself as dead. Let it
         free itself otherwise by marking its parent as dead. */
      if (!__atomic_compare_exchange_n (&proc_state->parent_alive,
                                        &proc_state->child_alive,
                                        false,
                                        false,
                                        __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED))
        {
          free (proc_state);
        }
      tid = TID_ERROR;
    }
  else
    {
      list_push_back (&thread_current ()->child_list, &proc_state->elem);
    }
  
  return tid;
}

/* A thread function that loads a user process and starts it
   running. */
static void
start_process (void *process_args)
{
  struct process_args *args = (struct process_args *) process_args;
  struct intr_frame if_;
  bool success;

  /* Initialize interrupt frame and load executable. */
  struct thread *t = thread_current ();
  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;
  success = load (args->command, &if_.eip, &if_.esp);
  t->proc_state = args->proc_state;
  if (args->cwd != NULL)
    t->cwd = dir_reopen (args->cwd);
  else
    t->cwd = NULL;
  args->success = success;
  args->proc_state->tid = thread_current ()->tid;
  palloc_free_page (args->command);
  
  /* Must be done with args before PROCESS_EXECUTE may continue tp
     prevent a dangling reference */
  sema_up (&args->process_loaded_sem);
  
  /* If load failed, quit. */
  if (!success) 
    thread_exit ();

  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
  NOT_REACHED ();
}

/* Converts an element to a STRUCT PROCESS_STATE pointer. */
static inline struct process_state *
elem_to_proc_state (const struct list_elem *e)
{
  return list_entry (e, struct process_state, elem);
}

/* Returns TRUE if the PROCESS_STATE associated with a LIST_ELEM E
   has a tid equal to that passed into AUX */
static bool 
tid_matches (const struct list_elem *e, void *aux)
{
  return elem_to_proc_state (e)->tid == (tid_t) aux;
}

/* Waits for thread TID to die and returns its exit status.  If
   it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If TID is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given TID, returns -1
   immediately, without waiting. */
int
process_wait (tid_t child_tid) 
{
  struct thread *cur = thread_current ();
  struct list_elem *e;
  struct process_state *child_proc_state;

  e = list_search_first (&cur->child_list, tid_matches, (void *) child_tid);
  if (e == list_end (&cur->child_list))
    return -1;
  
  child_proc_state = elem_to_proc_state (e);
  sema_down (&child_proc_state->wait_sem);
  int exit_status = child_proc_state->exit_status;
  list_remove (&child_proc_state->elem);
  free (child_proc_state);
  
  return exit_status;
}

/* Free the current process's resources. */
void
process_exit (void)
{
  struct thread *cur = thread_current ();
  uint32_t *pd;
  close_all_files ();

  if (cur->exec_file != NULL)
    file_close (cur->exec_file);

  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  pd = cur->pagedir;
  if (pd != NULL) 
    {
      /* Correct ordering here is crucial.  We must set
         cur->pagedir to NULL before switching page directories,
         so that a timer interrupt can't switch back to the
         process page directory.  We must activate the base page
         directory before destroying the process's page
         directory, or our active page directory will be one
         that's been freed (and cleared). */
      cur->pagedir = NULL;
      pagedir_activate (NULL);
      pagedir_destroy (pd);
    }

  struct list_elem *e;  
  for (e = list_begin (&cur->child_list); e != list_end (&cur->child_list);
       e = list_next (e))
    {
      struct process_state *child_proc_state = elem_to_proc_state (e);      
      if (!__atomic_compare_exchange_n (&child_proc_state->parent_alive,
                                        &child_proc_state->child_alive,
                                        false,
                                        false,
                                        __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED))
        {
          free (child_proc_state);
        }
          
    }

  if (cur->proc_state == NULL) 
  {
    /* Thread is not a process */
    return;
  }

  printf ("%s: exit(%d)\n", cur->name, cur->proc_state->exit_status);
  if (!__atomic_compare_exchange_n (&cur->proc_state->child_alive, 
                                    &cur->proc_state->parent_alive,
                                    false,
                                    false,
                                    __ATOMIC_RELAXED,
                                    __ATOMIC_RELAXED))
    {
      free (cur->proc_state);
    }
  else 
    sema_up (&cur->proc_state->wait_sem);
}

/* Sets up the CPU for running user code in the current
   thread.
   This function is called on every context switch. */
void
process_activate (void)
{
  struct thread *t = thread_current ();

  /* Activate thread's page tables. */
  pagedir_activate (t->pagedir);

  /* Set thread's kernel stack for use in processing
     interrupts. */
  tss_update ();
}

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32   /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32   /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32   /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16   /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr
  {
    unsigned char e_ident[16];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
  };

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr
  {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
  };

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

static void *setup_stack (void **esp);
static bool place_arguments (const char *command, void *kpage, void **esp);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool
load (const char *command, void (**eip) (void), void **esp) 
{
  struct thread *t = thread_current ();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  /* Allocate and activate page directory. */
  t->pagedir = pagedir_create ();
  if (t->pagedir == NULL) 
    goto done;
  process_activate ();
  
  /* Open executable file. */
  file = filesys_open (t->name);
  t->exec_file = file;
    if (file == NULL) 
    {
      printf ("load: %s: open failed\n", t->name);
      goto done; 
    }
  file_deny_write (file);
  
  /* Read and verify executable header. */
  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024) 
    {
      printf ("load: %s: error loading executable\n", t->name);
      goto done; 
    }
  
  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++) 
    {
      struct Elf32_Phdr phdr;
      if (file_ofs < 0 || file_ofs > file_length (file))
        goto done;
      file_seek (file, file_ofs);
      
      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
        goto done;
      
      file_ofs += sizeof phdr;
      switch (phdr.p_type) 
        {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
          /* Ignore this segment. */
          break;
        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
          goto done;
        case PT_LOAD:
          if (validate_segment (&phdr, file)) 
            {
              bool writable = (phdr.p_flags & PF_W) != 0;
              uint32_t file_page = phdr.p_offset & ~PGMASK;
              uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
              uint32_t page_offset = phdr.p_vaddr & PGMASK;
              uint32_t read_bytes, zero_bytes;
              if (phdr.p_filesz > 0)
                {
                  /* Normal segment.
                     Read initial part from disk and zero the rest. */
                  read_bytes = page_offset + phdr.p_filesz;
                  zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
                                - read_bytes);
                }
              else 
                {
                  /* Entirely zero.
                     Don't read anything from disk. */
                  read_bytes = 0;
                  zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                }
              if (!load_segment (file, file_page, (void *) mem_page,
                                 read_bytes, zero_bytes, writable))
                goto done;
            }
          else
            goto done;
          break;
        }
    }

  /* Set up stack. */
  void *kpage = setup_stack (esp);
  if (kpage == NULL)
    goto done;
  
  /* Place arguments on stack. */
  if (!place_arguments (command, kpage, esp))
    goto done;

  /* Start address. */
  *eip = (void (*) (void)) ehdr.e_entry;
  success = true;

 done:
 /* We arrive here whether the load is successful or not. */ 
  return success;
}

/* load() helpers. */

static bool install_page (void *upage, void *kpage, bool writable);

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file) 
{
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK)) 
    return false; 

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off) file_length (file)) 
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz) 
    return false; 

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;
  
  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable) 
{
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (ofs % PGSIZE == 0);
 
  file_seek (file, ofs);
   
  while (read_bytes > 0 || zero_bytes > 0) 
    {
      /* Calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;

      /* Get a page of memory. */
      uint8_t *kpage = palloc_get_page (PAL_USER);
      if (kpage == NULL)
        return false;

      /* Load this page. */
      if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes)
        {
          palloc_free_page (kpage);
          return false; 
        }

      memset (kpage + page_read_bytes, 0, page_zero_bytes);

      /* Add the page to the process's address space. */
      if (!install_page (upage, kpage, writable)) 
        {
          palloc_free_page (kpage);
          return false; 
        }

      /* Advance. */
      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      upage += PGSIZE;
    }
  return true;
}

/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
static void *
setup_stack (void **esp) 
{
  uint8_t *kpage;
  bool success = false;

  kpage = palloc_get_page (PAL_USER | PAL_ZERO);
  if (kpage != NULL) 
    {
      success = install_page (((uint8_t *) PHYS_BASE) - PGSIZE, kpage, true);
      if (success)
        *esp = PHYS_BASE;
      else
      {
        palloc_free_page (kpage);
        return NULL;
      }
    }
  return kpage;
}

/* Places arguments onto the new process' stack. */
static bool
place_arguments (const char *command, void *kpage, void **esp)
{
  void *off_esp = kpage + PGSIZE;
  const char *ch = command;
  char **argv_buf = (char **) kpage;
  int argc = 0;

  for (;;)
  {
    while (*ch == ' ')
      ch++;
    unsigned token_length = strcspn (ch, " ");
    if (token_length == 0)
      break;
    off_esp -= (token_length + 1);
    if (off_esp < kpage)
      {
        palloc_free_page (kpage);
        return false;
      }
    *esp -= (token_length + 1);
    strlcpy (off_esp, ch, token_length + 1);
    argv_buf[argc++] = *esp;
    ch += token_length;
  }
  
  argv_buf[argc] = NULL;

  /* Word-align memory before pushing further. */
  off_esp = (void *) ROUND_DOWN ((uintptr_t) off_esp, 4);
  *esp = (void *) ROUND_DOWN ((uintptr_t) *esp, 4);
  
  /* Copy argv array into its proper location, zero its source,
     copy argv pointer and argc */
  unsigned bytes_to_copy = (argc + 1) * sizeof (char *);
  off_esp -= bytes_to_copy;

  /* Account for argc, argv, return address */
  if (off_esp - 3 * sizeof (int) < kpage)
    {
      palloc_free_page (kpage);
      return false;
    }
  *esp -= bytes_to_copy;
  memmove (off_esp, argv_buf, bytes_to_copy);
  memset (argv_buf, 0, bytes_to_copy);

  char ** argv = *esp;
  off_esp -= sizeof (char **);
  *esp -= sizeof (char **);
  *(char ***) off_esp = argv;
 
  off_esp -= sizeof (int);
  *esp -= sizeof (int);
  *(int *) off_esp = argc;

  off_esp -= sizeof (void *);
  *esp -= sizeof (void *);
  *(void **) off_esp = NULL;

  return true;
}

/* Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
static bool
install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page (t->pagedir, upage) == NULL
          && pagedir_set_page (t->pagedir, upage, kpage, writable));

}
