#include <kernel/syscall.h>
#include <kernel/isr.h>
#include <kernel/process.h>
#include <kernel/tty.h>
#include <stdio.h>

/*
 * sys_exit — terminate the calling process.
 *
 * EBX = exit status (currently unused, logged for debugging).
 * The process is marked ZOMBIE and will never be scheduled again.
 * Control does not return to the caller.
 */
static void sys_exit(trapframe *tf) {
    uint32_t status = tf->ebx;
    printf("[syscall] exit(%d) from PID %d\n", status, current_process->pid);

    current_process->state = PROC_ZOMBIE;

    /* Yield the CPU — the scheduler will pick someone else.
       We halt in a loop; the next timer IRQ triggers the switch. */
    for (;;) __asm__ volatile("hlt");
}

/*
 * sys_write — write bytes to a file descriptor.
 *
 * EBX = fd   (only fd=1 stdout supported for now)
 * ECX = buf  (pointer to user buffer — must be in accessible pages)
 * EDX = len  (number of bytes to write)
 *
 * Returns number of bytes written (in EAX), or -1 on bad fd.
 */
static int32_t sys_write(trapframe *tf) {
    uint32_t fd  = tf->ebx;
    const char *buf = (const char *)tf->ecx;
    uint32_t len = tf->edx;

    if (fd != 1) {
        return -1;
    }

    for (uint32_t i = 0; i < len; i++) {
        terminal_putchar(buf[i]);
    }

    return (int32_t)len;
}

/* Syscall dispatch table — indexed by syscall number. */
typedef int32_t (*syscall_fn)(trapframe *);

static syscall_fn syscall_table[NUM_SYSCALLS] = {
    [SYS_EXIT]  = (syscall_fn)sys_exit,
    [SYS_WRITE] = sys_write,
};

void syscall_dispatch(trapframe *tf) {
    uint32_t num = tf->eax;

    if (num >= NUM_SYSCALLS || !syscall_table[num]) {
        printf("[syscall] unknown syscall %d from PID %d\n",
               num, current_process->pid);
        tf->eax = (uint32_t)-1;
        return;
    }

    int32_t ret = syscall_table[num](tf);
    tf->eax = (uint32_t)ret;
}
