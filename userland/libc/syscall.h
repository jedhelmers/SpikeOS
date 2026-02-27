#ifndef _USER_SYSCALL_H
#define _USER_SYSCALL_H

/* Syscall numbers — must match kernel/include/kernel/syscall.h */
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

static inline int syscall0(int num) {
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(num) : "memory");
    return ret;
}

static inline int syscall1(int num, int a) {
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(num), "b"(a) : "memory");
    return ret;
}

static inline int syscall2(int num, int a, int b) {
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret)
                     : "a"(num), "b"(a), "c"(b) : "memory");
    return ret;
}

static inline int syscall3(int num, int a, int b, int c) {
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret)
                     : "a"(num), "b"(a), "c"(b), "d"(c) : "memory");
    return ret;
}

#endif
