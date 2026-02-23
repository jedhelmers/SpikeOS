#ifndef _CONDVAR_H
#define _CONDVAR_H

#include <kernel/wait.h>
#include <kernel/mutex.h>

/*
 * Condition variable — "wait until some condition is true."
 *
 * Usage:
 *   condvar_t cv = CONDVAR_INIT;
 *   mutex_t   m  = MUTEX_INIT;
 *
 *   // Waiter:
 *   mutex_lock(&m);
 *   while (!condition)
 *       condvar_wait(&cv, &m);   // releases m, sleeps, re-acquires m
 *   mutex_unlock(&m);
 *
 *   // Signaler:
 *   mutex_lock(&m);
 *   condition = 1;
 *   condvar_signal(&cv);         // wake one waiter
 *   mutex_unlock(&m);
 */

typedef struct condvar {
    wait_queue_t wq;
} condvar_t;

#define CONDVAR_INIT { .wq = WAIT_QUEUE_INIT }

void condvar_init(condvar_t *cv);
void condvar_wait(condvar_t *cv, mutex_t *m);   /* release m, sleep, re-acquire m */
void condvar_signal(condvar_t *cv);              /* wake one waiter */
void condvar_broadcast(condvar_t *cv);           /* wake all waiters */

#endif
