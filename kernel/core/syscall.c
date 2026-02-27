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
#include <kernel/net.h>
#include <kernel/virtio_gpu.h>
#include <string.h>
#include <stdio.h>

#define USER_STACK_VADDR 0xBFFFF000u

/* ------------------------------------------------------------------ */
/*  User pointer validation                                           */
/* ------------------------------------------------------------------ */

/* Reject pointers into kernel space. User processes (via int 0x80)
   must only pass addresses below KERNEL_VMA_OFFSET. */
static int bad_user_ptr(const void *ptr, uint32_t len) {
    if (!ptr) return 1;
    uint32_t addr = (uint32_t)ptr;
    if (addr >= KERNEL_VMA_OFFSET) return 1;
    if (len > 0 && addr + len > KERNEL_VMA_OFFSET) return 1;
    if (len > 0 && addr + len < addr) return 1;  /* overflow */
    return 0;
}

static int bad_user_string(const char *str) {
    if (!str) return 1;
    if ((uint32_t)str >= KERNEL_VMA_OFFSET) return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  SYS_EXIT (0) — terminate calling process                          */
/* ------------------------------------------------------------------ */

__attribute__((noreturn))
static int32_t sys_exit(trapframe *tf) {
    int32_t status = (int32_t)tf->ebx;

    current_process->exit_status = status;
    proc_kill(current_process->pid);

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

    if (bad_user_ptr(buf, len)) return -1;
    return fd_write(fd, buf, len);
}

/* ------------------------------------------------------------------ */
/*  SYS_READ (2) — read from file descriptor                         */
/* ------------------------------------------------------------------ */

static int32_t sys_read(trapframe *tf) {
    int fd       = (int)tf->ebx;
    void *buf    = (void *)tf->ecx;
    uint32_t len = tf->edx;

    if (bad_user_ptr(buf, len)) return -1;
    return fd_read(fd, buf, len);
}

/* ------------------------------------------------------------------ */
/*  SYS_OPEN (3) — open a file                                       */
/*  EBX = path, ECX = flags                                           */
/* ------------------------------------------------------------------ */

static int32_t sys_open(trapframe *tf) {
    const char *path = (const char *)tf->ebx;
    uint32_t flags   = tf->ecx;

    if (bad_user_string(path)) return -1;
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

    if (bad_user_string(path)) return -1;
    if (bad_user_ptr(buf, sizeof(struct spike_stat))) return -1;

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
/*  SYS_BRK (9) — adjust process break                               */
/*  EBX = new break address (0 = query current break)                 */
/*  Returns current/new break on success, old break on failure.       */
/* ------------------------------------------------------------------ */

static int32_t sys_brk(trapframe *tf) {
    uint32_t new_brk = tf->ebx;
    uint32_t cur_brk = current_process->brk;

    /* Kernel threads have no user address space */
    if (current_process->cr3 == 0) return -1;

    /* Query: return current break */
    if (new_brk == 0) return (int32_t)cur_brk;

    /* Reject: can't shrink below initial program image */
    if (new_brk < cur_brk) return (int32_t)cur_brk;

    /* Reject: can't grow into kernel space or stack region */
    if (new_brk >= USER_STACK_VADDR) return (int32_t)cur_brk;

    /* Page-align the new break upward */
    uint32_t new_brk_page = (new_brk + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint32_t cur_brk_page = (cur_brk + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    /* Map any new pages needed between old and new break */
    for (uint32_t pv = cur_brk_page; pv < new_brk_page; pv += PAGE_SIZE) {
        uint32_t frame = alloc_frame();
        if (frame == FRAME_ALLOC_FAIL) return (int32_t)cur_brk;

        if (pgdir_map_user_page(current_process->cr3, pv, frame,
                                PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER) != 0) {
            free_frame(frame);
            return (int32_t)cur_brk;
        }

        /* Zero the new page */
        uint8_t *p = (uint8_t *)temp_map(frame);
        memset(p, 0, PAGE_SIZE);
        temp_unmap();
    }

    current_process->brk = new_brk;
    return (int32_t)new_brk;
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

    if (bad_user_string(path)) return -1;

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

    /* status_ptr is nullable, but if non-NULL it must be in user space */
    if (status_ptr && bad_user_ptr(status_ptr, sizeof(int32_t))) return -1;

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
    if (bad_user_string(path)) return -1;
    return vfs_mkdir(path) < 0 ? -1 : 0;
}

/* ------------------------------------------------------------------ */
/*  SYS_UNLINK (13) — remove a file or empty directory                */
/*  EBX = path                                                        */
/* ------------------------------------------------------------------ */

static int32_t sys_unlink(trapframe *tf) {
    const char *path = (const char *)tf->ebx;
    if (bad_user_string(path)) return -1;
    return (int32_t)vfs_remove(path);
}

/* ------------------------------------------------------------------ */
/*  SYS_CHDIR (14) — change working directory                         */
/*  EBX = path                                                        */
/* ------------------------------------------------------------------ */

static int32_t sys_chdir(trapframe *tf) {
    const char *path = (const char *)tf->ebx;
    if (bad_user_string(path)) return -1;
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

    if (bad_user_ptr(buf, bufsize)) return -1;

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
    if (bad_user_ptr(fds, 2 * sizeof(int))) return -1;

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
/*  SYS_SOCKET (19) — create a UDP socket                            */
/*  EBX = type (SOCK_UDP = 1)                                        */
/* ------------------------------------------------------------------ */

static int32_t sys_socket(trapframe *tf) {
    uint32_t type = tf->ebx;
    if (type != SOCK_UDP) return -1;

    /* udp_bind(0) allocates a socket without binding a port yet.
       We use a temporary high port so the slot is reserved. */
    return -1;  /* placeholder — sockets are created at bind time */
}

/* ------------------------------------------------------------------ */
/*  SYS_BIND (20) — bind a socket to a local port                   */
/*  EBX = type (SOCK_UDP), ECX = port                                */
/*  Returns socket index, or -1 on failure.                          */
/* ------------------------------------------------------------------ */

static int32_t sys_bind(trapframe *tf) {
    uint32_t type = tf->ebx;
    uint16_t port = (uint16_t)tf->ecx;

    if (type != SOCK_UDP) return -1;
    return (int32_t)udp_bind(port);
}

/* ------------------------------------------------------------------ */
/*  SYS_SENDTO (21) — send a UDP datagram                           */
/*  EBX = sock, ECX = pointer to struct sendto_args                  */
/* ------------------------------------------------------------------ */

static int32_t sys_sendto(trapframe *tf) {
    int sock = (int)tf->ebx;
    struct sendto_args *args = (struct sendto_args *)tf->ecx;

    if (bad_user_ptr(args, sizeof(struct sendto_args))) return -1;
    if (bad_user_ptr(args->buf, args->len)) return -1;

    return (int32_t)udp_sendto(sock, args->dst_ip, args->dst_port,
                                args->buf, args->len);
}

/* ------------------------------------------------------------------ */
/*  SYS_RECVFROM (22) — receive a UDP datagram (blocks)              */
/*  EBX = sock, ECX = pointer to struct recvfrom_args                */
/* ------------------------------------------------------------------ */

static int32_t sys_recvfrom(trapframe *tf) {
    int sock = (int)tf->ebx;
    struct recvfrom_args *args = (struct recvfrom_args *)tf->ecx;

    if (bad_user_ptr(args, sizeof(struct recvfrom_args))) return -1;
    if (bad_user_ptr(args->buf, args->max_len)) return -1;

    uint32_t from_ip = 0;
    uint16_t from_port = 0;
    int ret = udp_recv(sock, args->buf, args->max_len, &from_ip, &from_port);

    if (ret >= 0) {
        args->from_ip   = from_ip;
        args->from_port = from_port;
        args->received  = (uint16_t)ret;
    }

    return (int32_t)ret;
}

/* ------------------------------------------------------------------ */
/*  SYS_CLOSESOCK (23) — close a socket                              */
/*  EBX = sock                                                       */
/* ------------------------------------------------------------------ */

static int32_t sys_closesock(trapframe *tf) {
    int sock = (int)tf->ebx;
    udp_unbind(sock);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  SYS_MMAP (24) — map anonymous memory into process address space   */
/*  EBX = pointer to struct mmap_args                                 */
/*  Returns mapped address, or (uint32_t)-1 on failure.               */
/* ------------------------------------------------------------------ */

/* mmap region base — anonymous mappings start here and grow up */
#define MMAP_BASE 0x40000000u

static int32_t sys_mmap(trapframe *tf) {
    struct mmap_args *args = (struct mmap_args *)tf->ebx;

    if (bad_user_ptr(args, sizeof(struct mmap_args))) return -1;

    /* Kernel threads have no user address space */
    if (current_process->cr3 == 0) return -1;

    uint32_t length = args->length;
    uint32_t flags  = args->flags;
    uint32_t prot   = args->prot;
    uint32_t addr   = args->addr;

    /* Must be anonymous for now (no file-backed mappings) */
    if (!(flags & MAP_ANONYMOUS)) return -1;

    /* Length must be nonzero */
    if (length == 0) return -1;

    /* Page-align length upward */
    length = (length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    /* Check VMA table capacity */
    if (current_process->vma_count >= MAX_VMAS) return -1;

    /* Find a free address range */
    if (flags & MAP_FIXED) {
        /* MAP_FIXED: use exact address, must be page-aligned and in user space */
        if (addr & (PAGE_SIZE - 1)) return -1;
        if (addr < MMAP_BASE || addr + length > USER_STACK_VADDR) return -1;
        if (addr + length < addr) return -1;  /* overflow */

        /* Check for overlap with existing VMAs */
        for (uint32_t i = 0; i < current_process->vma_count; i++) {
            vma_t *v = &current_process->vmas[i];
            if (addr < v->base + v->length && addr + length > v->base)
                return -1;  /* overlap */
        }
    } else {
        /* Kernel chooses: find first gap starting from MMAP_BASE */
        addr = MMAP_BASE;

        /* Retry if overlapping — simple linear scan */
        int found = 0;
        for (int attempt = 0; attempt < 1000 && !found; attempt++) {
            found = 1;
            for (uint32_t i = 0; i < current_process->vma_count; i++) {
                vma_t *v = &current_process->vmas[i];
                if (addr < v->base + v->length && addr + length > v->base) {
                    /* Overlap — skip past this VMA */
                    addr = (v->base + v->length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
                    found = 0;
                    break;
                }
            }
        }
        if (!found) return -1;

        /* Bounds check */
        if (addr + length > USER_STACK_VADDR || addr + length < addr)
            return -1;
    }

    /* Allocate physical frames and map pages */
    uint32_t pages_mapped = 0;
    uint32_t page_flags = PAGE_PRESENT | PAGE_USER;
    if (prot & PROT_WRITE)
        page_flags |= PAGE_WRITABLE;

    for (uint32_t off = 0; off < length; off += PAGE_SIZE) {
        uint32_t frame = alloc_frame();
        if (frame == FRAME_ALLOC_FAIL) goto fail_unmap;

        if (pgdir_map_user_page(current_process->cr3, addr + off, frame,
                                page_flags) != 0) {
            free_frame(frame);
            goto fail_unmap;
        }

        /* Zero the page */
        uint8_t *p = (uint8_t *)temp_map(frame);
        memset(p, 0, PAGE_SIZE);
        temp_unmap();

        pages_mapped++;
    }

    /* Record VMA */
    vma_t *vma = &current_process->vmas[current_process->vma_count++];
    vma->base   = addr;
    vma->length = length;
    vma->prot   = prot;
    vma->flags  = flags;

    return (int32_t)addr;

fail_unmap:
    /* Roll back: unmap and free already-mapped pages */
    for (uint32_t i = 0; i < pages_mapped; i++) {
        uint32_t va = addr + i * PAGE_SIZE;
        uint32_t phys = virt_to_phys(va);
        if (phys != 0 && phys != FRAME_ALLOC_FAIL) {
            free_frame(phys);
        }
        /* Clear the PTE (map to 0 with no flags effectively unmaps) */
        pgdir_map_user_page(current_process->cr3, va, 0, 0);
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/*  SYS_MUNMAP (25) — unmap memory region                             */
/*  EBX = address, ECX = length                                       */
/*  Returns 0 on success, -1 on failure.                              */
/* ------------------------------------------------------------------ */

static int32_t sys_munmap(trapframe *tf) {
    uint32_t addr   = tf->ebx;
    uint32_t length = tf->ecx;

    /* Kernel threads have no user address space */
    if (current_process->cr3 == 0) return -1;

    /* Must be page-aligned */
    if (addr & (PAGE_SIZE - 1)) return -1;
    if (length == 0) return -1;

    /* Page-align length upward */
    length = (length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    /* Find the VMA that matches this range */
    int vma_idx = -1;
    for (uint32_t i = 0; i < current_process->vma_count; i++) {
        if (current_process->vmas[i].base == addr &&
            current_process->vmas[i].length == length) {
            vma_idx = (int)i;
            break;
        }
    }
    if (vma_idx < 0) return -1;  /* no matching VMA */

    /* Unmap pages and free frames */
    for (uint32_t off = 0; off < length; off += PAGE_SIZE) {
        uint32_t va = addr + off;
        uint32_t phys = virt_to_phys(va);
        if (phys != 0 && phys != FRAME_ALLOC_FAIL) {
            free_frame(phys);
        }
        pgdir_map_user_page(current_process->cr3, va, 0, 0);
        hal_tlb_invalidate(va);
    }

    /* Remove VMA entry by shifting the rest down */
    for (uint32_t i = (uint32_t)vma_idx; i + 1 < current_process->vma_count; i++) {
        current_process->vmas[i] = current_process->vmas[i + 1];
    }
    current_process->vma_count--;

    return 0;
}

/* ------------------------------------------------------------------ */
/*  SYS_GPU_CREATE_CTX (26) — create a VirGL 3D rendering context     */
/*  EBX = context ID (1-255)                                          */
/*  ECX = debug name string (or NULL)                                 */
/*  Returns 0 on success, -1 on failure.                              */
/* ------------------------------------------------------------------ */

static int32_t sys_gpu_create_ctx(trapframe *tf) {
    uint32_t ctx_id = tf->ebx;
    const char *name = (const char *)tf->ecx;

    if (ctx_id == 0 || ctx_id > 255) return -1;
    if (name && bad_user_string(name)) return -1;
    if (!virtio_gpu_has_virgl()) return -1;

    return (int32_t)virtio_gpu_ctx_create(ctx_id, name ? name : "user");
}

/* ------------------------------------------------------------------ */
/*  SYS_GPU_SUBMIT (27) — submit a VirGL command buffer               */
/*  EBX = pointer to struct gpu_submit_args                           */
/*  Returns 0 on success, -1 on failure.                              */
/* ------------------------------------------------------------------ */

static int32_t sys_gpu_submit(trapframe *tf) {
    struct gpu_submit_args *args = (struct gpu_submit_args *)tf->ebx;

    if (bad_user_ptr(args, sizeof(struct gpu_submit_args))) return -1;
    if (!virtio_gpu_has_virgl()) return -1;

    uint32_t ctx_id = args->ctx_id;
    const uint32_t *cmdbuf = args->cmdbuf;
    uint32_t size_bytes = args->size_bytes;

    if (ctx_id == 0 || ctx_id > 255) return -1;
    if (size_bytes == 0 || size_bytes > 65536) return -1;
    if (bad_user_ptr(cmdbuf, size_bytes)) return -1;

    return (int32_t)virtio_gpu_submit_3d(ctx_id, cmdbuf, size_bytes);
}

/* ------------------------------------------------------------------ */
/*  SYS_GPU_DESTROY_CTX (28) — destroy a VirGL context                */
/*  EBX = context ID                                                  */
/*  Returns 0 on success, -1 on failure.                              */
/* ------------------------------------------------------------------ */

static int32_t sys_gpu_destroy_ctx(trapframe *tf) {
    uint32_t ctx_id = tf->ebx;

    if (ctx_id == 0 || ctx_id > 255) return -1;
    if (!virtio_gpu_has_virgl()) return -1;

    return (int32_t)virtio_gpu_ctx_destroy(ctx_id);
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
    [SYS_KILL]      = sys_kill,
    [SYS_SOCKET]    = sys_socket,
    [SYS_BIND]      = sys_bind,
    [SYS_SENDTO]    = sys_sendto,
    [SYS_RECVFROM]  = sys_recvfrom,
    [SYS_CLOSESOCK] = sys_closesock,
    [SYS_MMAP]      = sys_mmap,
    [SYS_MUNMAP]    = sys_munmap,
    [SYS_GPU_CREATE_CTX]  = sys_gpu_create_ctx,
    [SYS_GPU_SUBMIT]      = sys_gpu_submit,
    [SYS_GPU_DESTROY_CTX] = sys_gpu_destroy_ctx,
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
