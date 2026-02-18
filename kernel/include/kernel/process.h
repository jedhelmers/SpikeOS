#ifndef _PROCESS_H
#define _PROCESS_H

#include <stdint.h>
#include <stddef.h>

#define MAX_PROCS 32

struct trapframe;   // forward declaration

enum proc_state {
    PROC_NEW,
    PROC_READY,
    PROC_RUNNING,
    PROC_BLOCKED,
    PROC_ZOMBIE,
};

struct cpu_context {
    uint32_t esp;   // kernel stack pointer
    uint32_t ebp;
};

struct process {
    uint32_t pid;
    enum proc_state state;

    uint32_t kstack_top;

    struct cpu_context ctx;     // schedule saves this
    struct trapframe *tf;       // last interrupt
    struct page_dir *mm;        // ionde? Am I there yet?
};

extern struct process *current_process;
extern struct process proc_table[MAX_PROCS];

struct process *process_ge_table(void);

void process_init(void);

struct process *proc_create_kernel_thread(void (*entry)(void));
void proc_kill(uint32_t pid);

#endif