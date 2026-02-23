#ifndef _PIPE_H
#define _PIPE_H

#include <stdint.h>
#include <kernel/wait.h>

/*
 * Kernel pipe — circular buffer with blocking read/write.
 *
 * A pipe has a read end and a write end, each exposed as an fd.
 * Reading from an empty pipe blocks. Writing to a full pipe blocks.
 * When all write-end fds are closed, reading returns 0 (EOF).
 * When all read-end fds are closed, writing returns -1 (broken pipe).
 */

#define PIPE_BUF_SIZE 512
#define MAX_PIPES     16

typedef struct pipe {
    uint8_t      buf[PIPE_BUF_SIZE];
    uint32_t     read_pos;    /* next byte to read */
    uint32_t     write_pos;   /* next byte to write */
    uint32_t     count;       /* bytes currently in buffer */
    int          readers;     /* number of open read-end fds */
    int          writers;     /* number of open write-end fds */
    wait_queue_t read_wq;     /* readers wait here when empty */
    wait_queue_t write_wq;    /* writers wait here when full */
    int          active;      /* 1 = in use, 0 = free */
} pipe_t;

/* Initialize the pipe subsystem */
void pipe_init(void);

/* Create a pipe. Returns 0 on success and fills read_fd/write_fd.
   Returns -1 on failure. Operates on current_process->fds. */
int pipe_create(int *read_fd, int *write_fd);

/* Read from a pipe. Blocks if empty, returns 0 on EOF. */
int32_t pipe_read(pipe_t *p, void *buf, uint32_t count);

/* Write to a pipe. Blocks if full, returns -1 on broken pipe. */
int32_t pipe_write(pipe_t *p, const void *buf, uint32_t count);

/* Called when an fd pointing to a pipe is closed */
void pipe_close_reader(pipe_t *p);
void pipe_close_writer(pipe_t *p);

#endif
