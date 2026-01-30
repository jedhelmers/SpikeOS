#include <kernel/scheduler.h>
#include <kernel/process.h>
#include <kernel/isr.h>
#include <stddef.h>

// Round-robbin
static uint32_t sched_index = 0;
static uint32_t sched_ticks = 0;

static struct process *pick_next(void) {
    struct process *table = process_ge_table();

    for (int i = 1; i <= MAX_PROCS; i++) {
        int idx = (sched_index + i) % MAX_PROCS;

        if (table[idx].state == PROC_READY) {
            sched_index = idx;

            return &table[idx];
        }
    }

    return current_process;
}

void scheduler_init(void) {
    // Setup
    sched_index = 0;
    sched_ticks = 0;
}

void scheduler_tick(trapframe *tf) {
    struct process *prev = current_process;
    struct process *next = pick_next();

    if (next == prev)
        return;

    prev->state = PROC_READY;
    next->state = PROC_RUNNING;
    current_process = next;

    // Redirect iret to next thread
    tf->eip = next->tf->eip;
    tf->cs  = next->tf->cs;
    tf->eflags = next->tf->eflags;
}
