#ifndef _USER_UNISTD_H
#define _USER_UNISTD_H

#include "syscall.h"
#include "stat.h"

static inline void _exit(int status) {
    syscall1(SYS_EXIT, status);
    for (;;);
}

static inline int write(int fd, const void *buf, int len) {
    return syscall3(SYS_WRITE, fd, (int)buf, len);
}

static inline int read(int fd, void *buf, int len) {
    return syscall3(SYS_READ, fd, (int)buf, len);
}

static inline int open(const char *path, int flags) {
    return syscall2(SYS_OPEN, (int)path, flags);
}

static inline int close(int fd) {
    return syscall1(SYS_CLOSE, fd);
}

static inline int getpid(void) {
    return syscall0(SYS_GETPID);
}

static inline int spike_pipe(int fds[2]) {
    return syscall1(SYS_PIPE, (int)fds);
}

static inline int dup(int fd) {
    return syscall1(SYS_DUP, fd);
}

static inline int spawn(const char *path) {
    return syscall1(SYS_SPAWN, (int)path);
}

static inline int waitpid(int pid) {
    return syscall2(SYS_WAITPID, pid, 0);
}

static inline int kill(int pid, int sig) {
    return syscall2(SYS_KILL, pid, sig);
}

static inline int spike_sleep(int ticks) {
    return syscall1(SYS_SLEEP, ticks);
}

static inline int chdir(const char *path) {
    return syscall1(SYS_CHDIR, (int)path);
}

static inline int mkdir(const char *path) {
    return syscall1(SYS_MKDIR, (int)path);
}

static inline int unlink(const char *path) {
    return syscall1(SYS_UNLINK, (int)path);
}

/* brk() — set the program break.
   Returns 0 on success, -1 on failure. */
static inline int brk(void *addr) {
    int result = syscall1(SYS_BRK, (int)addr);
    if (addr != (void *)0 && result != (int)addr)
        return -1;
    return 0;
}

/* sbrk() — increment the program break by 'incr' bytes.
   Returns pointer to the OLD break (start of new memory), or (void*)-1 on failure. */
static inline void *sbrk(int incr) {
    int cur = syscall1(SYS_BRK, 0);
    if (incr == 0)
        return (void *)cur;
    int new_brk = cur + incr;
    int result = syscall1(SYS_BRK, new_brk);
    if (result != new_brk)
        return (void *)-1;
    return (void *)cur;
}

static inline int lseek(int fd, int offset, int whence) {
    return syscall3(SYS_SEEK, fd, offset, whence);
}

static inline char *getcwd(char *buf, int size) {
    int result = syscall2(SYS_GETCWD, (int)buf, size);
    if (result < 0) return (char *)0;
    return buf;
}

static inline int stat(const char *path, struct spike_stat *buf) {
    return syscall2(SYS_STAT, (int)path, (int)buf);
}

#endif
