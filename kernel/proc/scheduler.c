#include <kernel/scheduler.h>
#include <kernel/process.h>
#include <kernel/isr.h>
#include <kernel/tss.h>
#include <kernel/hal.h>
#include <stddef.h>

// Round-robbin
static uint32_t sched_index = 0;
static uint32_t sched_ticks = 0;

static struct process *pick_next(void) {
    for (int step = 1; step <= MAX_PROCS; step++) {
        int idx = (sched_index + step) % MAX_PROCS;
        if (proc_table[idx].state == PROC_READY) {
            sched_index = idx;
            return &proc_table[idx];
        }
    }
    return current_process;
}


void scheduler_init(void) {
    // Setup
    sched_index = 0;
    sched_ticks = 0;
}

uint32_t scheduler_tick(trapframe *tf) {
    struct process *prev = current_process;

    // Save where prev can resume: the current trapframe address
    prev->tf = tf;
    prev->ctx.esp = (uint32_t)tf;

    // Put prev back on the run queue unless it's idle or not runnable
    if (prev != &proc_table[0] && prev->state == PROC_RUNNING) {
        prev->state = PROC_READY;
    }

    struct process *next = pick_next();

    // If nobody else is ready, keep running prev
    if (next == prev) {
        if (prev->state == PROC_READY) prev->state = PROC_RUNNING;
        return 0;
    }

    next->state = PROC_RUNNING;
    current_process = next;

    // Update TSS esp0 so the CPU uses the correct kernel stack
    // when this process is interrupted from ring 3.
    tss_set_kernel_stack(next->kstack_top);

    // Switch CR3 if the next process uses a different page directory
    uint32_t next_cr3 = proc_get_cr3(next);
    uint32_t prev_cr3 = proc_get_cr3(prev);
    if (next_cr3 != prev_cr3) {
        hal_set_cr3(next_cr3);
    }

    // Return the stack pointer of the next process's interrupt frame
    return next->ctx.esp;
}

