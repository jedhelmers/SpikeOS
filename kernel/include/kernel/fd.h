#ifndef _FD_H
#define _FD_H

#include <stdint.h>
#include <stddef.h>

/*
 * File descriptor subsystem.
 *
 * Each process gets its own fd table (MAX_FDS entries). An fd points to
 * a shared open_file struct which holds the inode, offset, and flags.
 * Multiple fds can point to the same open_file (e.g. after dup).
 *
 * Special file types:
 *   FD_TYPE_NONE    - slot is free
 *   FD_TYPE_VFS     - regular VFS file (inode-based)
 *   FD_TYPE_CONSOLE - stdin/stdout/stderr (terminal)
 *   FD_TYPE_PIPE    - pipe endpoint
 */

#define MAX_FDS         16     /* per-process fd limit */
#define MAX_OPEN_FILES  64     /* system-wide open file limit */

#define FD_TYPE_NONE    0
#define FD_TYPE_VFS     1
#define FD_TYPE_CONSOLE 2
#define FD_TYPE_PIPE    3

/* Flags for open_file */
#define O_RDONLY  0x0
#define O_WRONLY  0x1
#define O_RDWR   0x2
#define O_CREAT  0x100
#define O_TRUNC  0x200
#define O_APPEND 0x400

struct pipe;  /* forward declaration */

typedef struct open_file {
    uint8_t   type;       /* FD_TYPE_* */
    uint32_t  flags;      /* O_RDONLY, O_WRONLY, O_RDWR, etc. */
    uint32_t  ino;        /* VFS inode number (for FD_TYPE_VFS) */
    uint32_t  offset;     /* current read/write position */
    int       refcount;   /* number of fds pointing here */
    struct pipe *pipe;    /* pipe pointer (for FD_TYPE_PIPE) */
} open_file_t;

/* System-wide open file table */
extern open_file_t open_file_table[MAX_OPEN_FILES];

/* Initialize the open file table */
void fd_init(void);

/* Allocate an open_file slot. Returns index or -1. */
int alloc_open_file(void);

/* Release an open_file slot (decrements refcount, frees if 0). */
void release_open_file(int idx);

/* Allocate the lowest free fd in the given fd table. Returns fd or -1. */
int alloc_fd(int *fd_table);

/* Per-process fd operations (operate on current_process->fds) */
int     fd_open(const char *path, uint32_t flags);
int     fd_close(int fd);
int32_t fd_read(int fd, void *buf, uint32_t count);
int32_t fd_write(int fd, const void *buf, uint32_t count);
int32_t fd_seek(int fd, int32_t offset, int whence);

/* Seek whence values */
#define SEEK_SET  0
#define SEEK_CUR  1
#define SEEK_END  2

/* Initialize fd table for a new process (sets up stdin/stdout/stderr) */
void fd_init_process(int *fd_table);

/* Close all fds in a process's fd table */
void fd_close_all(int *fd_table);

#endif
