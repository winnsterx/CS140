#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

/* Maximum characters in a filename written by readdir(). */
#define READDIR_MAX_LEN 14

void syscall_init (void);

void close_all_files (void);
#endif /* userprog/syscall.h */
