#include <kernel/mutex.h>
#include <kernel/process.h>
#include <kernel/hal.h>

/* ------------------------------------------------------------------ */
/*  Spinlock                                                           */
/* ------------------------------------------------------------------ */

void spin_init(spinlock_t *s) {
    s->locked = 0;
    s->saved_flags = 0;
}

void spin_lock(spinlock_t *s) {
    s->saved_flags = hal_irq_save();
    s->locked = 1;
}

void spin_unlock(spinlock_t *s) {
    s->locked = 0;
    hal_irq_restore(s->saved_flags);
}

/* ------------------------------------------------------------------ */
/*  Mutex                                                              */
/* ------------------------------------------------------------------ */

void mutex_init(mutex_t *m) {
    m->locked = 0;
    m->wq.head = 0;
    m->owner = 0;
}

void mutex_lock(mutex_t *m) {
    while (1) {
        uint32_t flags = hal_irq_save();
        if (!m->locked) {
            m->locked = 1;
            m->owner = current_process;
            hal_irq_restore(flags);
            return;
        }
        hal_irq_restore(flags);
        sleep_on(&m->wq);
    }
}

void mutex_unlock(mutex_t *m) {
    uint32_t flags = hal_irq_save();
    m->locked = 0;
    m->owner = 0;
    hal_irq_restore(flags);
    wake_up_one(&m->wq);
}

int mutex_trylock(mutex_t *m) {
    uint32_t flags = hal_irq_save();
    if (!m->locked) {
        m->locked = 1;
        m->owner = current_process;
        hal_irq_restore(flags);
        return 1;
    }
    hal_irq_restore(flags);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Semaphore                                                          */
/* ------------------------------------------------------------------ */

void sem_init(semaphore_t *s, int initial_count) {
    s->count = initial_count;
    s->wq.head = 0;
}

void sem_wait(semaphore_t *s) {
    while (1) {
        uint32_t flags = hal_irq_save();
        if (s->count > 0) {
            s->count--;
            hal_irq_restore(flags);
            return;
        }
        hal_irq_restore(flags);
        sleep_on(&s->wq);
    }
}

void sem_post(semaphore_t *s) {
    uint32_t flags = hal_irq_save();
    s->count++;
    hal_irq_restore(flags);
    wake_up_one(&s->wq);
}

int sem_trywait(semaphore_t *s) {
    uint32_t flags = hal_irq_save();
    if (s->count > 0) {
        s->count--;
        hal_irq_restore(flags);
        return 1;
    }
    hal_irq_restore(flags);
    return 0;
}
