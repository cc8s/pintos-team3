#include <stdbool.h>
#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

struct thread;

void syscall_init (void);
void sys_exit (int status);
bool sys_copy_file_list (struct thread *child, struct thread *parent);

void sys_file_lock_acquire (void);
void sys_file_lock_release (void);

#endif /* userprog/syscall.h */
