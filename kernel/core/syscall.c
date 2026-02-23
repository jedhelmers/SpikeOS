#include <kernel/syscall.h>
#include <kernel/isr.h>
#include <kernel/process.h>
#include <kernel/paging.h>
#include <kernel/hal.h>
#include <kernel/fd.h>
#include <kernel/vfs.h>
#include <kernel/pipe.h>
#include <kernel/wait.h>
#include <kernel/timer.h>
#include <kernel/tty.h>
#include <kernel/signal.h>
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/*  SYS_EXIT (0) — terminate calling process                          */
/* ------------------------------------------------------------------ */

__attribute__((noreturn))
static int32_t sys_exit(trapframe *tf) {
    int32_t status = (int32_t)tf->ebx;

    current_process->exit_status = status;
    current_process->state = PROC_ZOMBIE;

    /* Close all file descriptors */
    fd_close_all(current_process->fds);

    /* Free per-process page directory */
    if (current_process->cr3 != 0) {
        hal_set_cr3(get_kernel_cr3());
        pgdir_destroy(current_process->cr3);
        current_process->cr3 = 0;
    }

    /* Wake parent if it's waiting on waitpid */
    for (int i = 0; i < MAX_PROCS; i++) {
        if (proc_table[i].pid == current_process->parent_pid &&
            proc_table[i].state != PROC_ZOMBIE) {
            wake_up_all(&proc_table[i].wait_children);
            break;
        }
    }

    hal_irq_enable();
    for (;;) hal_halt();
}

/* ------------------------------------------------------------------ */
/*  SYS_WRITE (1) — write to file descriptor                         */
/* ------------------------------------------------------------------ */

static int32_t sys_write(trapframe *tf) {
    int fd            = (int)tf->ebx;
    const void *buf   = (const void *)tf->ecx;
    uint32_t len      = tf->edx;

    return fd_write(fd, buf, len);
}

/* ------------------------------------------------------------------ */
/*  SYS_READ (2) — read from file descriptor                         */
/* ------------------------------------------------------------------ */

static int32_t sys_read(trapframe *tf) {
    int fd       = (int)tf->ebx;
    void *buf    = (void *)tf->ecx;
    uint32_t len = tf->edx;

    return fd_read(fd, buf, len);
}

/* ------------------------------------------------------------------ */
/*  SYS_OPEN (3) — open a file                                       */
/*  EBX = path, ECX = flags                                           */
/* ------------------------------------------------------------------ */

static int32_t sys_open(trapframe *tf) {
    const char *path = (const char *)tf->ebx;
    uint32_t flags   = tf->ecx;

    return (int32_t)fd_open(path, flags);
}

/* ------------------------------------------------------------------ */
/*  SYS_CLOSE (4) — close a file descriptor                          */
/* ------------------------------------------------------------------ */

static int32_t sys_close(trapframe *tf) {
    int fd = (int)tf->ebx;

    return (int32_t)fd_close(fd);
}

/* ------------------------------------------------------------------ */
/*  SYS_SEEK (5) — seek within a file                                */
/*  EBX = fd, ECX = offset, EDX = whence                             */
/* ------------------------------------------------------------------ */

static int32_t sys_seek(trapframe *tf) {
    int fd         = (int)tf->ebx;
    int32_t offset = (int32_t)tf->ecx;
    int whence     = (int)tf->edx;

    return fd_seek(fd, offset, whence);
}

/* ------------------------------------------------------------------ */
/*  SYS_STAT (6) — get file info                                     */
/*  EBX = path, ECX = pointer to struct spike_stat                    */
/* ------------------------------------------------------------------ */

static int32_t sys_stat(trapframe *tf) {
    const char *path       = (const char *)tf->ebx;
    struct spike_stat *buf = (struct spike_stat *)tf->ecx;

    int32_t ino = vfs_resolve(path, NULL, NULL);
    if (ino < 0) return -1;

    vfs_inode_t *node = vfs_get_inode((uint32_t)ino);
    if (!node) return -1;

    buf->type  = node->type;
    buf->size  = node->size;
    buf->ino   = (uint32_t)ino;
    buf->nlink = node->link_count;

    return 0;
}

/* ------------------------------------------------------------------ */
/*  SYS_GETPID (7) — get current process ID                          */
/* ------------------------------------------------------------------ */

static int32_t sys_getpid(trapframe *tf) {
    (void)tf;
    return (int32_t)current_process->pid;
}

/* ------------------------------------------------------------------ */
/*  SYS_SLEEP (8) — sleep for N ticks (10ms each at 100Hz)           */
/*  EBX = number of ticks                                             */
/* ------------------------------------------------------------------ */

static int32_t sys_sleep(trapframe *tf) {
    uint32_t ticks = tf->ebx;
    uint32_t target = timer_ticks() + ticks;

    /* Busy-wait with HLT — the scheduler still runs us on timer ticks,
       but we yield each time. This is simple and correct. */
    while (timer_ticks() < target) {
        hal_irq_enable();
        hal_halt();
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  SYS_BRK (9) — adjust process break (stub for now)                */
/*  EBX = new break address (0 = query current break)                 */
/* ------------------------------------------------------------------ */

static int32_t sys_brk(trapframe *tf) {
    (void)tf;
    /* Stub: user-mode heap not implemented yet */
    return -1;
}

/* ------------------------------------------------------------------ */
/*  SYS_SPAWN (10) — spawn a new process from an ELF in the VFS      */
/*  EBX = path to ELF file                                           */
/*  Returns child PID, or -1 on failure.                              */
/* ------------------------------------------------------------------ */

/* Forward declaration — implemented in elf_loader.c */
struct process *elf_spawn(const char *name);

static int32_t sys_spawn(trapframe *tf) {
    const char *path = (const char *)tf->ebx;

    struct process *child = elf_spawn(path);
    if (!child) return -1;

    child->parent_pid = current_process->pid;
    return (int32_t)child->pid;
}

/* ------------------------------------------------------------------ */
/*  SYS_WAITPID (11) — wait for a child to exit                      */
/*  EBX = child PID (or -1 for any child)                             */
/*  ECX = pointer to int32_t status (or NULL)                         */
/*  Returns exited child's PID, or -1 on error.                      */
/* ------------------------------------------------------------------ */

static int32_t sys_waitpid(trapframe *tf) {
    int32_t target_pid   = (int32_t)tf->ebx;
    int32_t *status_ptr  = (int32_t *)tf->ecx;

    while (1) {
        /* Search for a zombie child matching the target */
        for (int i = 1; i < MAX_PROCS; i++) {
            if (proc_table[i].state != PROC_ZOMBIE) continue;
            if (proc_table[i].parent_pid != current_process->pid) continue;
            if (target_pid > 0 && proc_table[i].pid != (uint32_t)target_pid)
                continue;

            /* Found a zombie child — reap it */
            uint32_t child_pid = proc_table[i].pid;
            int32_t  child_status = proc_table[i].exit_status;

            if (status_ptr) *status_ptr = child_status;

            /* Mark slot as fully reusable */
            proc_table[i].pid = 0;

            return (int32_t)child_pid;
        }

        /* Check if we have any living children to wait for */
        int has_children = 0;
        for (int i = 1; i < MAX_PROCS; i++) {
            if (proc_table[i].parent_pid == current_process->pid &&
                proc_table[i].state != PROC_ZOMBIE &&
                proc_table[i].pid != 0) {
                has_children = 1;
                break;
            }
        }
        if (!has_children) return -1;

        /* Block until a child exits */
        sleep_on(&current_process->wait_children);
    }
}

/* ------------------------------------------------------------------ */
/*  SYS_MKDIR (12) — create a directory                               */
/*  EBX = path                                                        */
/* ------------------------------------------------------------------ */

static int32_t sys_mkdir(trapframe *tf) {
    const char *path = (const char *)tf->ebx;
    return vfs_mkdir(path);
}

/* ------------------------------------------------------------------ */
/*  SYS_UNLINK (13) — remove a file or empty directory                */
/*  EBX = path                                                        */
/* ------------------------------------------------------------------ */

static int32_t sys_unlink(trapframe *tf) {
    const char *path = (const char *)tf->ebx;
    return (int32_t)vfs_remove(path);
}

/* ------------------------------------------------------------------ */
/*  SYS_CHDIR (14) — change working directory                         */
/*  EBX = path                                                        */
/* ------------------------------------------------------------------ */

static int32_t sys_chdir(trapframe *tf) {
    const char *path = (const char *)tf->ebx;
    return (int32_t)vfs_chdir(path);
}

/* ------------------------------------------------------------------ */
/*  SYS_GETCWD (15) — get current working directory                   */
/*  EBX = buffer, ECX = buffer size                                   */
/*  Returns 0 on success, -1 on failure.                              */
/* ------------------------------------------------------------------ */

static int32_t sys_getcwd(trapframe *tf) {
    char *buf        = (char *)tf->ebx;
    uint32_t bufsize = tf->ecx;

    const char *cwd = vfs_get_cwd_path();
    uint32_t len = (uint32_t)strlen(cwd) + 1;
    if (len > bufsize) return -1;

    memcpy(buf, cwd, len);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  SYS_PIPE (16) — create a pipe                                     */
/*  EBX = int[2] array to receive read/write fds                      */
/* ------------------------------------------------------------------ */

static int32_t sys_pipe(trapframe *tf) {
    int *fds = (int *)tf->ebx;
    if (!fds) return -1;

    int read_fd, write_fd;
    if (pipe_create(&read_fd, &write_fd) != 0)
        return -1;

    fds[0] = read_fd;
    fds[1] = write_fd;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  SYS_DUP (17) — duplicate a file descriptor                        */
/*  EBX = fd to duplicate                                              */
/*  Returns new fd, or -1 on failure.                                  */
/* ------------------------------------------------------------------ */

static int32_t sys_dup(trapframe *tf) {
    int old_fd = (int)tf->ebx;
    if (old_fd < 0 || old_fd >= MAX_FDS) return -1;

    int ofi = current_process->fds[old_fd];
    if (ofi < 0) return -1;

    int new_fd = alloc_fd(current_process->fds);
    if (new_fd < 0) return -1;

    current_process->fds[new_fd] = ofi;
    open_file_table[ofi].refcount++;

    return new_fd;
}

/* ------------------------------------------------------------------ */
/*  SYS_KILL (18) — send a signal to a process                       */
/*  EBX = pid, ECX = signal number                                    */
/* ------------------------------------------------------------------ */

static int32_t sys_kill(trapframe *tf) {
    uint32_t pid = tf->ebx;
    int sig      = (int)tf->ecx;

    return (int32_t)proc_signal(pid, sig);
}

/* ------------------------------------------------------------------ */
/*  Dispatch table                                                    */
/* ------------------------------------------------------------------ */

typedef int32_t (*syscall_fn)(trapframe *);

static syscall_fn syscall_table[NUM_SYSCALLS] = {
    [SYS_EXIT]    = sys_exit,
    [SYS_WRITE]   = sys_write,
    [SYS_READ]    = sys_read,
    [SYS_OPEN]    = sys_open,
    [SYS_CLOSE]   = sys_close,
    [SYS_SEEK]    = sys_seek,
    [SYS_STAT]    = sys_stat,
    [SYS_GETPID]  = sys_getpid,
    [SYS_SLEEP]   = sys_sleep,
    [SYS_BRK]     = sys_brk,
    [SYS_SPAWN]   = sys_spawn,
    [SYS_WAITPID] = sys_waitpid,
    [SYS_MKDIR]   = sys_mkdir,
    [SYS_UNLINK]  = sys_unlink,
    [SYS_CHDIR]   = sys_chdir,
    [SYS_GETCWD]  = sys_getcwd,
    [SYS_PIPE]    = sys_pipe,
    [SYS_DUP]     = sys_dup,
    [SYS_KILL]    = sys_kill,
};

void syscall_dispatch(trapframe *tf) {
    uint32_t num = tf->eax;

    if (num >= NUM_SYSCALLS || !syscall_table[num]) {
        printf("[syscall] unknown syscall %d from PID %d\n",
               num, current_process->pid);
        tf->eax = (uint32_t)-1;
        return;
    }

    int32_t ret = syscall_table[num](tf);
    tf->eax = (uint32_t)ret;

    /* Check for pending signals before returning to user mode */
    signal_check_pending();
}
