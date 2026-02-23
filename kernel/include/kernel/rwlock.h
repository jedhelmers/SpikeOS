#ifndef _RWLOCK_H
#define _RWLOCK_H

#include <kernel/wait.h>

/*
 * Read-write lock — multiple concurrent readers OR one exclusive writer.
 *
 * Writer starvation prevention: new readers block while a writer is pending.
 *
 * Usage:
 *   rwlock_t rw = RWLOCK_INIT;
 *
 *   // Reader:
 *   rwlock_read_lock(&rw);
 *   ... read shared data ...
 *   rwlock_read_unlock(&rw);
 *
 *   // Writer:
 *   rwlock_write_lock(&rw);
 *   ... modify shared data ...
 *   rwlock_write_unlock(&rw);
 */

typedef struct rwlock {
    volatile int reader_count;     /* number of active readers */
    volatile int writer_active;    /* 1 if a writer holds the lock */
    volatile int writer_pending;   /* number of writers waiting */
    wait_queue_t read_wq;          /* readers wait here */
    wait_queue_t write_wq;         /* writers wait here */
} rwlock_t;

#define RWLOCK_INIT { .reader_count = 0, .writer_active = 0, .writer_pending = 0, \
                      .read_wq = WAIT_QUEUE_INIT, .write_wq = WAIT_QUEUE_INIT }

void rwlock_init(rwlock_t *rw);
void rwlock_read_lock(rwlock_t *rw);
void rwlock_read_unlock(rwlock_t *rw);
void rwlock_write_lock(rwlock_t *rw);
void rwlock_write_unlock(rwlock_t *rw);

#endif
