#include <kernel/process.h>
#include <kernel/isr.h>
#include <string.h>
#include <stdio.h>


#define KSTACK_SIZE 4096
static uint8_t kstacks[MAX_PROCS][KSTACK_SIZE];

static uint8_t idle_stack[KSTACK_SIZE];
static uint32_t next_pid = 1;

struct process *current_process = NULL;
struct process proc_table[MAX_PROCS];

void process_init(void) {
    // Clear process table
    for (int i = 0; i < MAX_PROCS; i++) {
        proc_table[i].pid = 0;
        proc_table[i].state = PROC_ZOMBIE;
        proc_table[i].tf = NULL;
        proc_table[i].mm = NULL;
    }

    // Initialize idle/kernel process (PID 0)
    struct process *idle = &proc_table[0];

    idle->pid = 0;
    idle->state = PROC_RUNNING;
    idle->mm = NULL;

    // Setup kernel stack - grows down
    idle->kstack_top = (uint32_t)&idle_stack[KSTACK_SIZE];

    idle->ctx.esp = idle->kstack_top;
    idle->ctx.ebp = idle->kstack_top;

    idle->tf = NULL;

    current_process = idle;
}

struct process *process_ge_table(void) {
    return proc_table;
}

struct process *proc_create_kernel_thread(void (*entry)(void)) {
    for (int i = 1; i < MAX_PROCS; i++) {
        if (proc_table[i].state == PROC_ZOMBIE) {
            struct process *p = &proc_table[i];

            p->pid = next_pid++;
            p->state = PROC_READY;
            p->mm = NULL;

            // Assign a real kernel stack for this process
            p->kstack_top = (uint32_t)&kstacks[i][KSTACK_SIZE];
            uint32_t top = p->kstack_top;

            // Build a synthetic trapframe at the top of the stack
            top -= sizeof(trapframe);
            trapframe *tf = (trapframe *)top;

            memset(tf, 0, sizeof(*tf));

            tf->cs = 0x08;
            tf->ds = tf->es = tf->fs = tf->gs = 0x10;

            tf->eflags = 0x00000202;     // IF=1
            tf->eip = (uint32_t)entry;

            tf->esp_dummy = (uint32_t)&tf->int_no;

            tf->int_no = 0;
            tf->err_code = 0;

            p->tf = tf;
            p->ctx.esp = (uint32_t)tf;
            p->ctx.ebp = (uint32_t)tf;

            p->state = PROC_READY;

            printf("Thread %x stack top: %x\n", p->pid, p->kstack_top);

            return p;
        }
    }

    return NULL;
}

