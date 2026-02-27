#ifndef _PROCESS_H
#define _PROCESS_H

#include <stdint.h>
#include <stddef.h>
#include <kernel/fd.h>
#include <kernel/wait.h>

#define MAX_PROCS 32
#define MAX_VMAS  16        /* max mmap'd regions per process */

/* Virtual Memory Area — tracks one mmap'd region */
typedef struct {
    uint32_t base;          /* page-aligned start address */
    uint32_t length;        /* byte length (page-aligned) */
    uint32_t prot;          /* PROT_READ | PROT_WRITE | PROT_EXEC */
    uint32_t flags;         /* MAP_ANONYMOUS | MAP_PRIVATE | MAP_SHARED */
} vma_t;

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

    uint32_t cr3;               // physical address of page directory
                                // 0 = use kernel's page directory

    /* File descriptor table (indexes into open_file_table, -1 = free) */
    int fds[MAX_FDS];

    /* Process hierarchy */
    uint32_t parent_pid;        /* PID of parent (0 = no parent / init) */
    int32_t  exit_status;       /* set on exit, read by waitpid */
    wait_queue_t wait_children; /* parent sleeps here for waitpid */

    uint32_t cwd;               /* inode of current working directory */
    uint32_t pending_signals;   /* bitmask of pending signals */
    uint32_t brk;               /* program break (end of user heap) */

    /* Virtual memory areas — tracks mmap'd regions */
    vma_t    vmas[MAX_VMAS];    /* per-process VMA table */
    uint32_t vma_count;         /* number of active VMAs */
};

extern struct process *current_process;
extern struct process proc_table[MAX_PROCS];

struct process *process_ge_table(void);

void process_init(void);

struct process *proc_create_kernel_thread(void (*entry)(void));
struct process *proc_create_user_process(uint32_t pd_phys, uint32_t user_eip, uint32_t user_esp);
void proc_kill(uint32_t pid);

uint32_t get_kernel_cr3(void);
uint32_t proc_get_cr3(struct process *p);

#endif