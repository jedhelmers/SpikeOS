#ifndef _WAIT_H
#define _WAIT_H

#include <stdint.h>

/*
 * Wait queue — linked list of processes blocked on a resource.
 *
 * Usage:
 *   wait_queue_t wq = WAIT_QUEUE_INIT;
 *   sleep_on(&wq);        // blocks current process
 *   wake_up_one(&wq);     // wakes one waiter
 *   wake_up_all(&wq);     // wakes all waiters
 */

struct process;

typedef struct wait_queue_entry {
    struct process           *proc;
    struct wait_queue_entry  *next;
} wait_queue_entry_t;

typedef struct wait_queue {
    wait_queue_entry_t *head;
} wait_queue_t;

#define WAIT_QUEUE_INIT { .head = 0 }

/* Block the current process on this wait queue.
   The caller must be in a context where scheduling is safe
   (interrupts will be re-enabled). */
void sleep_on(wait_queue_t *wq);

/* Wake one waiting process (FIFO order). Returns 1 if a process was woken. */
int wake_up_one(wait_queue_t *wq);

/* Wake all waiting processes. Returns number of processes woken. */
int wake_up_all(wait_queue_t *wq);

#endif
