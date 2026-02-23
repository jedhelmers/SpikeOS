#include <kernel/condvar.h>
#include <kernel/hal.h>

void condvar_init(condvar_t *cv) {
    cv->wq.head = 0;
}

void condvar_wait(condvar_t *cv, mutex_t *m) {
    /* Release the mutex before sleeping.
       Must unlock manually (not via mutex_unlock) to avoid
       wake_up_one on the mutex wq racing with our sleep_on. */
    uint32_t flags = hal_irq_save();
    m->locked = 0;
    m->owner = 0;
    hal_irq_restore(flags);
    wake_up_one(&m->wq);    /* Let a contender acquire the mutex */

    sleep_on(&cv->wq);      /* Block until signaled */

    mutex_lock(m);           /* Re-acquire the mutex */
}

void condvar_signal(condvar_t *cv) {
    wake_up_one(&cv->wq);
}

void condvar_broadcast(condvar_t *cv) {
    wake_up_all(&cv->wq);
}
