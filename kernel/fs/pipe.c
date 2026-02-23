#include <kernel/pipe.h>
#include <kernel/fd.h>
#include <kernel/process.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Pipe pool                                                         */
/* ------------------------------------------------------------------ */

static pipe_t pipe_pool[MAX_PIPES];

void pipe_init(void) {
    memset(pipe_pool, 0, sizeof(pipe_pool));
}

static pipe_t *alloc_pipe(void) {
    for (int i = 0; i < MAX_PIPES; i++) {
        if (!pipe_pool[i].active) {
            memset(&pipe_pool[i], 0, sizeof(pipe_t));
            pipe_pool[i].active = 1;
            return &pipe_pool[i];
        }
    }
    return (pipe_t *)0;
}

/* ------------------------------------------------------------------ */
/*  pipe_create                                                       */
/* ------------------------------------------------------------------ */

int pipe_create(int *read_fd, int *write_fd) {
    pipe_t *p = alloc_pipe();
    if (!p) return -1;

    p->readers = 1;
    p->writers = 1;

    /* Allocate read-end open file */
    int ofi_r = alloc_open_file();
    if (ofi_r < 0) { p->active = 0; return -1; }
    open_file_table[ofi_r].type = FD_TYPE_PIPE;
    open_file_table[ofi_r].flags = O_RDONLY;
    open_file_table[ofi_r].pipe = p;

    /* Allocate write-end open file */
    int ofi_w = alloc_open_file();
    if (ofi_w < 0) { release_open_file(ofi_r); p->active = 0; return -1; }
    open_file_table[ofi_w].type = FD_TYPE_PIPE;
    open_file_table[ofi_w].flags = O_WRONLY;
    open_file_table[ofi_w].pipe = p;

    /* Allocate fds in current process */
    int rfd = alloc_fd(current_process->fds);
    if (rfd < 0) {
        release_open_file(ofi_r);
        release_open_file(ofi_w);
        p->active = 0;
        return -1;
    }
    current_process->fds[rfd] = ofi_r;

    int wfd = alloc_fd(current_process->fds);
    if (wfd < 0) {
        current_process->fds[rfd] = -1;
        release_open_file(ofi_r);
        release_open_file(ofi_w);
        p->active = 0;
        return -1;
    }
    current_process->fds[wfd] = ofi_w;

    *read_fd = rfd;
    *write_fd = wfd;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  pipe_read                                                         */
/* ------------------------------------------------------------------ */

int32_t pipe_read(pipe_t *p, void *buf, uint32_t count) {
    uint8_t *out = (uint8_t *)buf;
    uint32_t total = 0;

    while (total < count) {
        /* Wait while buffer is empty and writers exist */
        while (p->count == 0 && p->writers > 0) {
            sleep_on(&p->read_wq);
        }

        /* If empty and no writers left, return what we have (EOF) */
        if (p->count == 0 && p->writers <= 0) {
            break;
        }

        /* Copy available bytes */
        while (total < count && p->count > 0) {
            out[total++] = p->buf[p->read_pos];
            p->read_pos = (p->read_pos + 1) % PIPE_BUF_SIZE;
            p->count--;
        }

        /* Wake any blocked writers */
        wake_up_all(&p->write_wq);
    }

    return (int32_t)total;
}

/* ------------------------------------------------------------------ */
/*  pipe_write                                                        */
/* ------------------------------------------------------------------ */

int32_t pipe_write(pipe_t *p, const void *buf, uint32_t count) {
    const uint8_t *in = (const uint8_t *)buf;
    uint32_t total = 0;

    while (total < count) {
        /* No readers left → broken pipe */
        if (p->readers <= 0) {
            return total > 0 ? (int32_t)total : -1;
        }

        /* Wait while buffer is full */
        while (p->count >= PIPE_BUF_SIZE && p->readers > 0) {
            sleep_on(&p->write_wq);
        }

        if (p->readers <= 0) {
            return total > 0 ? (int32_t)total : -1;
        }

        /* Copy bytes into buffer */
        while (total < count && p->count < PIPE_BUF_SIZE) {
            p->buf[p->write_pos] = in[total++];
            p->write_pos = (p->write_pos + 1) % PIPE_BUF_SIZE;
            p->count++;
        }

        /* Wake any blocked readers */
        wake_up_all(&p->read_wq);
    }

    return (int32_t)total;
}

/* ------------------------------------------------------------------ */
/*  Close endpoints                                                   */
/* ------------------------------------------------------------------ */

void pipe_close_reader(pipe_t *p) {
    p->readers--;
    if (p->readers <= 0) {
        /* Wake blocked writers so they get -1 */
        wake_up_all(&p->write_wq);
    }
    if (p->readers <= 0 && p->writers <= 0) {
        p->active = 0;
    }
}

void pipe_close_writer(pipe_t *p) {
    p->writers--;
    if (p->writers <= 0) {
        /* Wake blocked readers so they get EOF */
        wake_up_all(&p->read_wq);
    }
    if (p->readers <= 0 && p->writers <= 0) {
        p->active = 0;
    }
}
