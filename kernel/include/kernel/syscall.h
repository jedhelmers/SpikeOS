#ifndef _SYSCALL_H
#define _SYSCALL_H

#include <stdint.h>

struct trapframe;

/*
 * Syscall numbers — passed in EAX by user code before `int $0x80`.
 *
 * Convention:
 *   EAX = syscall number
 *   EBX = arg1
 *   ECX = arg2
 *   EDX = arg3
 *   Return value written back to trapframe EAX.
 */
#define SYS_EXIT   0
#define SYS_WRITE  1

#define NUM_SYSCALLS 2

/* Dispatch a syscall based on the trapframe registers.
   Called from isr_common_handler when int_no == 0x80. */
void syscall_dispatch(struct trapframe *tf);

#endif
