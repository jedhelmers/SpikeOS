#ifndef _USER_PTHREAD_H
#define _USER_PTHREAD_H

/*
 * Minimal pthreads stubs for single-threaded userland.
 *
 * SpikeOS does not yet support multiple threads per process.
 * These stubs allow libraries like Mesa to link, but everything
 * runs in a single thread. Mutexes are no-ops, thread creation
 * returns EAGAIN, and TLS keys use a static array.
 */

/* Thread ID type */
typedef unsigned int pthread_t;

/* Mutex types */
typedef struct {
    int locked;
} pthread_mutex_t;

typedef struct {
    int type;
} pthread_mutexattr_t;

/* Condition variable */
typedef struct {
    int dummy;
} pthread_cond_t;

typedef struct {
    int dummy;
} pthread_condattr_t;

/* Thread attributes */
typedef struct {
    int detachstate;
} pthread_attr_t;

/* Once control */
typedef int pthread_once_t;
#define PTHREAD_ONCE_INIT 0

/* TLS key */
typedef unsigned int pthread_key_t;

/* Mutex initializer */
#define PTHREAD_MUTEX_INITIALIZER { 0 }
#define PTHREAD_COND_INITIALIZER  { 0 }

/* Mutex type constants */
#define PTHREAD_MUTEX_NORMAL     0
#define PTHREAD_MUTEX_RECURSIVE  1
#define PTHREAD_MUTEX_ERRORCHECK 2
#define PTHREAD_MUTEX_DEFAULT    PTHREAD_MUTEX_NORMAL

/* Detach state */
#define PTHREAD_CREATE_JOINABLE 0
#define PTHREAD_CREATE_DETACHED 1

/* Thread operations (stubs — single-threaded) */
int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start_routine)(void *), void *arg);
int pthread_join(pthread_t thread, void **retval);
int pthread_detach(pthread_t thread);
pthread_t pthread_self(void);
int pthread_equal(pthread_t t1, pthread_t t2);

/* Mutex operations (no-ops in single-threaded mode) */
int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr);
int pthread_mutex_destroy(pthread_mutex_t *mutex);
int pthread_mutex_lock(pthread_mutex_t *mutex);
int pthread_mutex_trylock(pthread_mutex_t *mutex);
int pthread_mutex_unlock(pthread_mutex_t *mutex);

/* Mutex attributes */
int pthread_mutexattr_init(pthread_mutexattr_t *attr);
int pthread_mutexattr_destroy(pthread_mutexattr_t *attr);
int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type);

/* Condition variable operations */
int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr);
int pthread_cond_destroy(pthread_cond_t *cond);
int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex);
int pthread_cond_signal(pthread_cond_t *cond);
int pthread_cond_broadcast(pthread_cond_t *cond);

/* Once */
int pthread_once(pthread_once_t *once_control, void (*init_routine)(void));

/* Thread-local storage (uses static array, max 64 keys) */
int pthread_key_create(pthread_key_t *key, void (*destructor)(void *));
int pthread_key_delete(pthread_key_t key);
void *pthread_getspecific(pthread_key_t key);
int pthread_setspecific(pthread_key_t key, const void *value);

#endif
