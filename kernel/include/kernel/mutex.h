#ifndef _MUTEX_H
#define _MUTEX_H

#include <stdint.h>
#include <kernel/wait.h>

/* ------------------------------------------------------------------ */
/*  Spinlock (interrupt-disabling, non-reentrant)                      */
/*  On a uniprocessor, disabling interrupts guarantees mutual          */
/*  exclusion — no CAS loop needed.                                    */
/* ------------------------------------------------------------------ */

typedef struct spinlock {
    volatile int locked;
    uint32_t saved_flags;
} spinlock_t;

#define SPINLOCK_INIT { .locked = 0, .saved_flags = 0 }

void spin_init(spinlock_t *s);
void spin_lock(spinlock_t *s);
void spin_unlock(spinlock_t *s);

/* ------------------------------------------------------------------ */
/*  Mutex (blocking, non-recursive)                                    */
/*  Uses wait queue for contention — same pattern as pipe blocking.    */
/* ------------------------------------------------------------------ */

struct process;

typedef struct mutex {
    volatile int locked;
    wait_queue_t wq;
    struct process *owner;
} mutex_t;

#define MUTEX_INIT { .locked = 0, .wq = WAIT_QUEUE_INIT, .owner = 0 }

void mutex_init(mutex_t *m);
void mutex_lock(mutex_t *m);
void mutex_unlock(mutex_t *m);
int  mutex_trylock(mutex_t *m);  /* returns 1 if acquired, 0 if not */

/* ------------------------------------------------------------------ */
/*  Counting Semaphore                                                 */
/* ------------------------------------------------------------------ */

typedef struct semaphore {
    volatile int count;
    wait_queue_t wq;
} semaphore_t;

void sem_init(semaphore_t *s, int initial_count);
void sem_wait(semaphore_t *s);    /* P / down — blocks if count <= 0 */
void sem_post(semaphore_t *s);    /* V / up   — increments, wakes one */
int  sem_trywait(semaphore_t *s); /* returns 1 if acquired, 0 if not */

#endif
