#include <kernel/fd.h>
#include <kernel/vfs.h>
#include <kernel/process.h>
#include <kernel/pipe.h>
#include <kernel/tty.h>
#include <kernel/keyboard.h>
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/*  System-wide open file table                                       */
/* ------------------------------------------------------------------ */

open_file_t open_file_table[MAX_OPEN_FILES];

void fd_init(void) {
    memset(open_file_table, 0, sizeof(open_file_table));
}

int alloc_open_file(void) {
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (open_file_table[i].type == FD_TYPE_NONE) {
            memset(&open_file_table[i], 0, sizeof(open_file_t));
            open_file_table[i].refcount = 1;
            return i;
        }
    }
    return -1;
}

void release_open_file(int idx) {
    if (idx < 0 || idx >= MAX_OPEN_FILES) return;
    open_file_t *of = &open_file_table[idx];

    of->refcount--;
    if (of->refcount <= 0) {
        /* Close pipe endpoints */
        if (of->type == FD_TYPE_PIPE && of->pipe) {
            if (of->flags & O_WRONLY)
                pipe_close_writer(of->pipe);
            else
                pipe_close_reader(of->pipe);
        }
        memset(of, 0, sizeof(open_file_t));
    }
}

int alloc_fd(int *fd_table) {
    for (int i = 0; i < MAX_FDS; i++) {
        if (fd_table[i] == -1) {
            return i;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/*  Per-process fd init                                                */
/* ------------------------------------------------------------------ */

void fd_init_process(int *fd_table) {
    /* Mark all fds as free */
    for (int i = 0; i < MAX_FDS; i++) {
        fd_table[i] = -1;
    }

    /* Allocate stdin (fd 0) — console, read-only */
    int of_in = alloc_open_file();
    if (of_in >= 0) {
        open_file_table[of_in].type = FD_TYPE_CONSOLE;
        open_file_table[of_in].flags = O_RDONLY;
        fd_table[0] = of_in;
    }

    /* Allocate stdout (fd 1) — console, write-only */
    int of_out = alloc_open_file();
    if (of_out >= 0) {
        open_file_table[of_out].type = FD_TYPE_CONSOLE;
        open_file_table[of_out].flags = O_WRONLY;
        fd_table[1] = of_out;
    }

    /* Allocate stderr (fd 2) — console, write-only */
    int of_err = alloc_open_file();
    if (of_err >= 0) {
        open_file_table[of_err].type = FD_TYPE_CONSOLE;
        open_file_table[of_err].flags = O_WRONLY;
        fd_table[2] = of_err;
    }
}

void fd_close_all(int *fd_table) {
    for (int i = 0; i < MAX_FDS; i++) {
        if (fd_table[i] != -1) {
            release_open_file(fd_table[i]);
            fd_table[i] = -1;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  fd operations                                                     */
/* ------------------------------------------------------------------ */

int fd_open(const char *path, uint32_t flags) {
    if (!path) return -1;

    /* Resolve the path */
    int32_t ino = vfs_resolve(path, NULL, NULL);

    if (ino < 0) {
        /* File doesn't exist — create if O_CREAT */
        if (flags & O_CREAT) {
            ino = vfs_create_file(path);
            if (ino < 0) return -1;
        } else {
            return -1;
        }
    }

    /* Check that it's a file */
    vfs_inode_t *node = vfs_get_inode((uint32_t)ino);
    if (!node || node->type != VFS_TYPE_FILE) return -1;

    /* Allocate open file entry */
    int ofi = alloc_open_file();
    if (ofi < 0) return -1;

    open_file_table[ofi].type = FD_TYPE_VFS;
    open_file_table[ofi].flags = flags;
    open_file_table[ofi].ino = (uint32_t)ino;
    open_file_table[ofi].offset = 0;

    /* Truncate if requested */
    if (flags & O_TRUNC) {
        vfs_inode_t *n = vfs_get_inode((uint32_t)ino);
        if (n) {
            n->size = 0;
        }
    }

    /* Allocate fd in current process */
    int fd = alloc_fd(current_process->fds);
    if (fd < 0) {
        release_open_file(ofi);
        return -1;
    }

    current_process->fds[fd] = ofi;
    return fd;
}

int fd_close(int fd) {
    if (fd < 0 || fd >= MAX_FDS) return -1;
    if (current_process->fds[fd] == -1) return -1;

    release_open_file(current_process->fds[fd]);
    current_process->fds[fd] = -1;
    return 0;
}

int32_t fd_read(int fd, void *buf, uint32_t count) {
    if (fd < 0 || fd >= MAX_FDS) return -1;
    int ofi = current_process->fds[fd];
    if (ofi < 0) return -1;

    open_file_t *of = &open_file_table[ofi];

    switch (of->type) {
    case FD_TYPE_CONSOLE: {
        /* Blocking character-at-a-time read from keyboard */
        uint8_t *out = (uint8_t *)buf;
        uint32_t total = 0;

        while (total < count) {
            key_event_t e = keyboard_get_event_blocking();

            switch (e.type) {
            case KEY_CHAR:
                out[total++] = (uint8_t)e.ch;
                break;
            case KEY_ENTER:
                out[total++] = '\n';
                break;
            case KEY_BACKSPACE:
                out[total++] = '\b';
                break;
            default:
                break;
            }

            /* Return after first character (raw mode) */
            if (total > 0) break;
        }
        return (int32_t)total;
    }

    case FD_TYPE_VFS: {
        int32_t n = vfs_read(of->ino, buf, of->offset, count);
        if (n > 0) of->offset += (uint32_t)n;
        return n;
    }

    case FD_TYPE_PIPE:
        if (!of->pipe) return -1;
        return pipe_read(of->pipe, buf, count);

    default:
        return -1;
    }
}

int32_t fd_write(int fd, const void *buf, uint32_t count) {
    if (fd < 0 || fd >= MAX_FDS) return -1;
    int ofi = current_process->fds[fd];
    if (ofi < 0) return -1;

    open_file_t *of = &open_file_table[ofi];

    switch (of->type) {
    case FD_TYPE_CONSOLE:
        terminal_write((const char *)buf, count);
        return (int32_t)count;

    case FD_TYPE_VFS: {
        if (of->flags & O_APPEND) {
            vfs_inode_t *node = vfs_get_inode(of->ino);
            if (node) of->offset = node->size;
        }
        int32_t n = vfs_write(of->ino, buf, of->offset, count);
        if (n > 0) of->offset += (uint32_t)n;
        return n;
    }

    case FD_TYPE_PIPE:
        if (!of->pipe) return -1;
        return pipe_write(of->pipe, buf, count);

    default:
        return -1;
    }
}

int32_t fd_seek(int fd, int32_t offset, int whence) {
    if (fd < 0 || fd >= MAX_FDS) return -1;
    int ofi = current_process->fds[fd];
    if (ofi < 0) return -1;

    open_file_t *of = &open_file_table[ofi];
    if (of->type != FD_TYPE_VFS) return -1;

    vfs_inode_t *node = vfs_get_inode(of->ino);
    if (!node) return -1;

    int32_t new_offset;
    switch (whence) {
    case SEEK_SET:
        new_offset = offset;
        break;
    case SEEK_CUR:
        new_offset = (int32_t)of->offset + offset;
        break;
    case SEEK_END:
        new_offset = (int32_t)node->size + offset;
        break;
    default:
        return -1;
    }

    if (new_offset < 0) return -1;
    of->offset = (uint32_t)new_offset;
    return new_offset;
}
