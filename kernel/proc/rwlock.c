#include <kernel/rwlock.h>
#include <kernel/hal.h>

void rwlock_init(rwlock_t *rw) {
    rw->reader_count = 0;
    rw->writer_active = 0;
    rw->writer_pending = 0;
    rw->read_wq.head = 0;
    rw->write_wq.head = 0;
}

void rwlock_read_lock(rwlock_t *rw) {
    while (1) {
        uint32_t flags = hal_irq_save();
        if (!rw->writer_active && rw->writer_pending == 0) {
            rw->reader_count++;
            hal_irq_restore(flags);
            return;
        }
        hal_irq_restore(flags);
        sleep_on(&rw->read_wq);
    }
}

void rwlock_read_unlock(rwlock_t *rw) {
    uint32_t flags = hal_irq_save();
    rw->reader_count--;
    int readers_left = rw->reader_count;
    hal_irq_restore(flags);

    if (readers_left == 0) {
        wake_up_one(&rw->write_wq);  /* Let a pending writer in */
    }
}

void rwlock_write_lock(rwlock_t *rw) {
    uint32_t flags = hal_irq_save();
    rw->writer_pending++;
    hal_irq_restore(flags);

    while (1) {
        flags = hal_irq_save();
        if (!rw->writer_active && rw->reader_count == 0) {
            rw->writer_active = 1;
            rw->writer_pending--;
            hal_irq_restore(flags);
            return;
        }
        hal_irq_restore(flags);
        sleep_on(&rw->write_wq);
    }
}

void rwlock_write_unlock(rwlock_t *rw) {
    uint32_t flags = hal_irq_save();
    rw->writer_active = 0;
    hal_irq_restore(flags);

    /* Wake all blocked readers first, then one writer */
    wake_up_all(&rw->read_wq);
    wake_up_one(&rw->write_wq);
}
