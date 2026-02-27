#ifndef _SYSCALL_H
#define _SYSCALL_H

#include <stdint.h>

struct trapframe;

/*
 * Syscall numbers — passed in EAX by user code before `int $0x80`.
 *
 * Convention:
 *   EAX = syscall number
 *   EBX = arg1
 *   ECX = arg2
 *   EDX = arg3
 *   Return value written back to trapframe EAX.
 */
#define SYS_EXIT     0
#define SYS_WRITE    1
#define SYS_READ     2
#define SYS_OPEN     3
#define SYS_CLOSE    4
#define SYS_SEEK     5
#define SYS_STAT     6
#define SYS_GETPID   7
#define SYS_SLEEP    8
#define SYS_BRK      9
#define SYS_SPAWN   10
#define SYS_WAITPID 11
#define SYS_MKDIR   12
#define SYS_UNLINK  13
#define SYS_CHDIR   14
#define SYS_GETCWD  15
#define SYS_PIPE    16
#define SYS_DUP     17
#define SYS_KILL      18
#define SYS_SOCKET    19
#define SYS_BIND      20
#define SYS_SENDTO    21
#define SYS_RECVFROM  22
#define SYS_CLOSESOCK 23
#define SYS_MMAP      24
#define SYS_MUNMAP    25
#define SYS_GPU_CREATE_CTX  26
#define SYS_GPU_SUBMIT      27
#define SYS_GPU_DESTROY_CTX 28

#define NUM_SYSCALLS  29

/* Argument struct for SYS_GPU_SUBMIT */
struct gpu_submit_args {
    uint32_t ctx_id;        /* VirGL context ID */
    const uint32_t *cmdbuf; /* pointer to command buffer (uint32 words) */
    uint32_t size_bytes;    /* size of command buffer in bytes */
};

/* mmap protection flags (prot argument) */
#define PROT_NONE   0x0
#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define PROT_EXEC   0x4

/* mmap flags */
#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x04
#define MAP_FIXED     0x08

/* mmap failure sentinel */
#define MAP_FAILED ((void *)-1)

/* Argument struct passed by pointer for SYS_MMAP (6 params > 3 registers) */
struct mmap_args {
    uint32_t addr;      /* hint or fixed address (0 = kernel chooses) */
    uint32_t length;    /* size in bytes (rounded up to PAGE_SIZE) */
    uint32_t prot;      /* PROT_READ | PROT_WRITE | PROT_EXEC */
    uint32_t flags;     /* MAP_ANONYMOUS | MAP_PRIVATE | MAP_SHARED */
    int32_t  fd;        /* file descriptor (-1 for anonymous) */
    uint32_t offset;    /* offset into file (must be page-aligned) */
};

/* Socket type for SYS_SOCKET */
#define SOCK_UDP  1

/* Argument structs passed by pointer for SYS_SENDTO / SYS_RECVFROM */
struct sendto_args {
    uint32_t    dst_ip;     /* network byte order */
    uint16_t    dst_port;   /* host byte order */
    const void *buf;
    uint16_t    len;
};

struct recvfrom_args {
    void     *buf;
    uint16_t  max_len;
    uint32_t  from_ip;     /* filled by kernel, network byte order */
    uint16_t  from_port;   /* filled by kernel, host byte order */
    uint16_t  received;    /* filled by kernel */
};

/*
 * Stat struct — returned by SYS_STAT.
 * Passed as a pointer in ECX.
 */
struct spike_stat {
    uint8_t  type;      /* VFS_TYPE_FILE / VFS_TYPE_DIR */
    uint32_t size;      /* file size in bytes */
    uint32_t ino;       /* inode number */
    uint16_t nlink;     /* link count */
};

/* Dispatch a syscall based on the trapframe registers.
   Called from isr_common_handler when int_no == 0x80. */
void syscall_dispatch(struct trapframe *tf);

#endif
