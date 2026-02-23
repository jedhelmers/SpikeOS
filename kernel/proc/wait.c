#include <kernel/wait.h>
#include <kernel/process.h>
#include <kernel/hal.h>
#include <stddef.h>

/*
 * Wait queue implementation.
 *
 * sleep_on() blocks the current process by setting it to PROC_BLOCKED,
 * adding it to the wait queue, then spinning on HLT until the scheduler
 * picks another process and this one is eventually woken.
 *
 * Wake functions set the process back to PROC_READY and remove it
 * from the queue.
 */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdangling-pointer"

void sleep_on(wait_queue_t *wq) {
    if (!wq || !current_process) return;

    /* Stack-allocated queue entry — valid as long as we're blocked on
       this function's stack frame, which we are (we don't return until
       woken). The compiler warns about dangling pointers but this is
       a standard kernel pattern (cf. Linux wait_event). */
    wait_queue_entry_t entry;
    entry.proc = current_process;
    entry.next = NULL;

    /* Disable interrupts while modifying the queue and process state */
    hal_irq_disable();

    /* Append to tail of queue */
    if (!wq->head) {
        wq->head = &entry;
    } else {
        wait_queue_entry_t *tail = wq->head;
        while (tail->next) tail = tail->next;
        tail->next = &entry;
    }

    current_process->state = PROC_BLOCKED;

    /* Re-enable interrupts and wait — the scheduler (on next timer tick)
       will see us as BLOCKED and skip us. When wake_up sets us back to
       PROC_READY, the scheduler will resume us here. */
    hal_irq_enable();
    while (current_process->state == PROC_BLOCKED) {
        hal_halt();
    }
}

#pragma GCC diagnostic pop

int wake_up_one(wait_queue_t *wq) {
    if (!wq) return 0;

    uint32_t flags = hal_irq_save();

    wait_queue_entry_t *entry = wq->head;
    if (entry) {
        wq->head = entry->next;
        entry->proc->state = PROC_READY;
        hal_irq_restore(flags);
        return 1;
    }

    hal_irq_restore(flags);
    return 0;
}

int wake_up_all(wait_queue_t *wq) {
    if (!wq) return 0;

    uint32_t flags = hal_irq_save();

    int count = 0;
    while (wq->head) {
        wait_queue_entry_t *entry = wq->head;
        wq->head = entry->next;
        entry->proc->state = PROC_READY;
        count++;
    }

    hal_irq_restore(flags);
    return count;
}
