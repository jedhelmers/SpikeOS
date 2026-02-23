#ifndef _SIGNAL_H
#define _SIGNAL_H

#include <stdint.h>

/* Signal numbers (matching POSIX where possible) */
#define SIGKILL   9
#define SIGSEGV  11
#define SIGPIPE  13

#define NSIG     32

/* Bitmask operations */
#define SIG_BIT(sig) (1u << ((sig) - 1))

/*
 * Send a signal to a process.
 * All signals are fatal in Tier 2 (non-catchable).
 * Returns 0 on success, -1 if process not found.
 */
int proc_signal(uint32_t pid, int sig);

/*
 * Check and deliver any pending signals on the current process.
 * Called from the syscall return path.
 * If a fatal signal is pending, the process is terminated.
 */
void signal_check_pending(void);

#endif
