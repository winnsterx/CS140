#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"

struct process_state
  {
    tid_t tid;
    bool child_alive;
    bool parent_alive;
    struct list_elem elem;
    struct semaphore wait_sem;
    struct lock status_lock;
    int exit_status;
  };

tid_t process_execute (const char *file_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (void);

#endif /* userprog/process.h */
