#ifndef _USER_STAT_H
#define _USER_STAT_H

/* Must match kernel's struct spike_stat in kernel/include/kernel/syscall.h */
struct spike_stat {
    unsigned char  type;      /* S_TYPE_FILE (1) / S_TYPE_DIR (2) */
    unsigned int   size;      /* file size in bytes */
    unsigned int   ino;       /* inode number */
    unsigned short nlink;     /* link count */
};

/* File types (match VFS_TYPE_* in kernel) */
#define S_TYPE_FILE  1
#define S_TYPE_DIR   2

/* Seek whence values (match kernel fd.h) */
#define SEEK_SET  0
#define SEEK_CUR  1
#define SEEK_END  2

/* Open flags (match kernel fd.h) */
#define O_RDONLY  0x0
#define O_WRONLY  0x1
#define O_RDWR   0x2
#define O_CREAT  0x100
#define O_TRUNC  0x200
#define O_APPEND 0x400

#endif
