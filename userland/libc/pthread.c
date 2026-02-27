/*
 * pthread.c — single-threaded pthreads stubs for SpikeOS.
 *
 * All operations succeed (return 0) except pthread_create which
 * returns EAGAIN. Mutexes track locked state but don't block since
 * there's only one thread. TLS uses a static value array.
 */

#include "pthread.h"
#include "errno.h"

/* ------------------------------------------------------------------ */
/*  Thread operations                                                  */
/* ------------------------------------------------------------------ */

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start_routine)(void *), void *arg) {
    (void)thread; (void)attr; (void)start_routine; (void)arg;
    return EAGAIN;  /* no multi-threading support */
}

int pthread_join(pthread_t thread, void **retval) {
    (void)thread; (void)retval;
    return ESRCH;  /* no threads to join */
}

int pthread_detach(pthread_t thread) {
    (void)thread;
    return 0;
}

pthread_t pthread_self(void) {
    return 1;  /* main thread = ID 1 */
}

int pthread_equal(pthread_t t1, pthread_t t2) {
    return t1 == t2;
}

/* ------------------------------------------------------------------ */
/*  Mutex operations (single-threaded — track state but never block)   */
/* ------------------------------------------------------------------ */

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr) {
    (void)attr;
    if (!mutex) return EINVAL;
    mutex->locked = 0;
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *mutex) {
    if (!mutex) return EINVAL;
    mutex->locked = 0;
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *mutex) {
    if (!mutex) return EINVAL;
    mutex->locked = 1;
    return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *mutex) {
    if (!mutex) return EINVAL;
    if (mutex->locked) return EBUSY;
    mutex->locked = 1;
    return 0;
}

int pthread_mutex_unlock(pthread_mutex_t *mutex) {
    if (!mutex) return EINVAL;
    mutex->locked = 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Mutex attributes                                                   */
/* ------------------------------------------------------------------ */

int pthread_mutexattr_init(pthread_mutexattr_t *attr) {
    if (!attr) return EINVAL;
    attr->type = PTHREAD_MUTEX_DEFAULT;
    return 0;
}

int pthread_mutexattr_destroy(pthread_mutexattr_t *attr) {
    (void)attr;
    return 0;
}

int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type) {
    if (!attr) return EINVAL;
    attr->type = type;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Condition variable (single-threaded — signal/broadcast are no-ops) */
/* ------------------------------------------------------------------ */

int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr) {
    (void)attr;
    if (!cond) return EINVAL;
    cond->dummy = 0;
    return 0;
}

int pthread_cond_destroy(pthread_cond_t *cond) {
    (void)cond;
    return 0;
}

int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
    /* In single-threaded mode, waiting on a condvar is a deadlock.
       Return immediately — the condition should already be true. */
    (void)cond; (void)mutex;
    return 0;
}

int pthread_cond_signal(pthread_cond_t *cond) {
    (void)cond;
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t *cond) {
    (void)cond;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Once                                                               */
/* ------------------------------------------------------------------ */

int pthread_once(pthread_once_t *once_control, void (*init_routine)(void)) {
    if (!once_control || !init_routine) return EINVAL;
    if (*once_control == 0) {
        *once_control = 1;
        init_routine();
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Thread-local storage (static array — single-threaded)              */
/* ------------------------------------------------------------------ */

#define PTHREAD_KEYS_MAX 64

static void *tls_values[PTHREAD_KEYS_MAX];
static void (*tls_destructors[PTHREAD_KEYS_MAX])(void *);
static int tls_used[PTHREAD_KEYS_MAX];
static unsigned int next_key = 0;

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *)) {
    if (!key) return EINVAL;

    for (unsigned int i = 0; i < PTHREAD_KEYS_MAX; i++) {
        unsigned int k = (next_key + i) % PTHREAD_KEYS_MAX;
        if (!tls_used[k]) {
            tls_used[k] = 1;
            tls_values[k] = 0;
            tls_destructors[k] = destructor;
            *key = k;
            next_key = k + 1;
            return 0;
        }
    }
    return EAGAIN;  /* no free keys */
}

int pthread_key_delete(pthread_key_t key) {
    if (key >= PTHREAD_KEYS_MAX || !tls_used[key]) return EINVAL;
    tls_used[key] = 0;
    tls_values[key] = 0;
    tls_destructors[key] = 0;
    return 0;
}

void *pthread_getspecific(pthread_key_t key) {
    if (key >= PTHREAD_KEYS_MAX || !tls_used[key]) return 0;
    return tls_values[key];
}

int pthread_setspecific(pthread_key_t key, const void *value) {
    if (key >= PTHREAD_KEYS_MAX || !tls_used[key]) return EINVAL;
    tls_values[key] = (void *)value;
    return 0;
}
