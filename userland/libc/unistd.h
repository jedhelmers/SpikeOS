#ifndef _USER_UNISTD_H
#define _USER_UNISTD_H

#include "syscall.h"

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

#endif
