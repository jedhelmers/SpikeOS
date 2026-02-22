#ifndef _VFS_H
#define _VFS_H

#include <stdint.h>
#include <stddef.h>

#define VFS_MAX_INODES_CAP 8192   /* hard ceiling for inode count */
#define VFS_MAX_NAME       59

#define VFS_TYPE_FREE   0
#define VFS_TYPE_FILE   1
#define VFS_TYPE_DIR    2

typedef struct vfs_inode {
    uint8_t   type;         /* VFS_TYPE_FREE / FILE / DIR */
    uint32_t  size;         /* bytes (file) or entry count (dir) */
    void     *data;         /* kmalloc'd: byte buffer (file) or dirent array (dir) */
    uint32_t  capacity;     /* allocated bytes (file) or dirent slots (dir) */
    uint16_t  link_count;   /* directory entries pointing to this inode */
} vfs_inode_t;

typedef struct vfs_dirent {
    char     name[VFS_MAX_NAME + 1]; /* NUL-terminated filename */
    uint32_t inode;                  /* index into inode table */
} vfs_dirent_t;

/* Initialization (initial_capacity = starting inode slots, grows on demand) */
void    vfs_init(uint32_t initial_capacity);
void    vfs_import_initrd(void);

/* Grow the inode table to at least min_inodes slots (krealloc). Returns 0 on success. */
int     vfs_ensure_capacity(uint32_t min_inodes);

/* Path resolution: resolve path to inode number (-1 if not found).
   If parent_out != NULL, resolves the parent dir and puts the leaf
   component name into leaf_out (must be VFS_MAX_NAME+1 bytes). */
int32_t vfs_resolve(const char *path, uint32_t *parent_out, char *leaf_out);

/* File/directory operations */
int32_t vfs_create_file(const char *path);
int32_t vfs_mkdir(const char *path);
int     vfs_remove(const char *path);
int32_t vfs_read(uint32_t ino, void *buf, uint32_t offset, uint32_t count);
int32_t vfs_write(uint32_t ino, const void *buf, uint32_t offset, uint32_t count);
int     vfs_rename(const char *old_path, const char *new_path);
int32_t vfs_copy(const char *src_path, const char *dst_path);

/* Directory listing (prints to terminal) */
int     vfs_list(uint32_t dir_ino);

/* Current working directory */
uint32_t vfs_get_cwd(void);
int      vfs_chdir(const char *path);
const char *vfs_get_cwd_path(void);

/* Access inode by number (for shell to inspect type/size) */
vfs_inode_t *vfs_get_inode(uint32_t ino);

/* Reset: free all inodes and data, re-init empty root (used by spikefs_load) */
void vfs_reset(void);

/* Runtime inode capacity (determined at init from disk size) */
uint32_t vfs_get_max_inodes(void);

/* Dirty tracking (Linux-style write-back support) */
int  vfs_is_dirty(void);
void vfs_mark_clean(void);

#endif
