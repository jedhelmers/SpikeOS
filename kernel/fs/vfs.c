#include <kernel/vfs.h>
#include <kernel/heap.h>
#include <kernel/initrd.h>
#include <kernel/paging.h>
#include <kernel/process.h>
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/*  Globals                                                           */
/* ------------------------------------------------------------------ */

static vfs_inode_t *inode_table = NULL;  /* heap-allocated, sized at init */
static uint32_t     num_inodes  = 0;     /* runtime capacity */
static uint32_t     cwd_inode   = 0;
static int          dirty       = 0;     /* set on any VFS mutation */

#define VFS_PATH_MAX   256
static char cwd_path_buf[VFS_PATH_MAX];

/* Per-process CWD: use process's cwd if available, else global fallback */
static uint32_t effective_cwd(void) {
    if (current_process)
        return current_process->cwd;
    return cwd_inode;  /* early boot, before process_init */
}

#define DIR_INIT_CAP   8   /* initial dirent slots per directory */

/* ------------------------------------------------------------------ */
/*  Inode allocation / free                                           */
/* ------------------------------------------------------------------ */

static int32_t inode_alloc(uint8_t type) {
    for (uint32_t i = 1; i < num_inodes; i++) {  /* 0 = root */
        if (inode_table[i].type == VFS_TYPE_FREE) {
            memset(&inode_table[i], 0, sizeof(vfs_inode_t));
            inode_table[i].type = type;
            dirty = 1;
            return (int32_t)i;
        }
    }

    /* Table full — try to grow (btrfs/XFS-style dynamic allocation) */
    uint32_t old_cap = num_inodes;
    uint32_t new_cap = old_cap * 2;
    if (new_cap > VFS_MAX_INODES_CAP) new_cap = VFS_MAX_INODES_CAP;
    if (new_cap <= old_cap) {
        printf("[vfs] inode table at max (%d inodes)\n", num_inodes);
        return -1;
    }
    if (vfs_ensure_capacity(new_cap) != 0) {
        printf("[vfs] failed to grow inode table\n");
        return -1;
    }
    printf("[vfs] grew inode table: %d -> %d\n", old_cap, num_inodes);
    /* Use first slot of the newly added region (guaranteed free) */
    memset(&inode_table[old_cap], 0, sizeof(vfs_inode_t));
    inode_table[old_cap].type = type;
    dirty = 1;
    return (int32_t)old_cap;
}

static void inode_free(uint32_t ino) {
    if (ino >= num_inodes) return;
    if (inode_table[ino].data)
        kfree(inode_table[ino].data);
    memset(&inode_table[ino], 0, sizeof(vfs_inode_t));
    dirty = 1;
}

/* ------------------------------------------------------------------ */
/*  Directory helpers                                                 */
/* ------------------------------------------------------------------ */

static int32_t dir_lookup(uint32_t dir_ino, const char *name) {
    vfs_inode_t *dir = &inode_table[dir_ino];
    if (dir->type != VFS_TYPE_DIR) return -1;

    vfs_dirent_t *entries = (vfs_dirent_t *)dir->data;
    for (uint32_t i = 0; i < dir->size; i++) {
        if (strcmp(entries[i].name, name) == 0)
            return (int32_t)entries[i].inode;
    }
    return -1;
}

static int dir_add_entry(uint32_t dir_ino, const char *name, uint32_t child_ino) {
    vfs_inode_t *dir = &inode_table[dir_ino];
    if (dir->type != VFS_TYPE_DIR) return -1;

    /* Grow array if needed */
    if (dir->size >= dir->capacity) {
        uint32_t new_cap = dir->capacity ? dir->capacity * 2 : DIR_INIT_CAP;
        vfs_dirent_t *new_arr = (vfs_dirent_t *)krealloc(
            dir->data, new_cap * sizeof(vfs_dirent_t));
        if (!new_arr) {
            printf("[vfs] dir_add_entry: out of memory\n");
            return -1;
        }
        dir->data = new_arr;
        dir->capacity = new_cap;
    }

    vfs_dirent_t *entries = (vfs_dirent_t *)dir->data;
    strncpy(entries[dir->size].name, name, VFS_MAX_NAME);
    entries[dir->size].name[VFS_MAX_NAME] = '\0';
    entries[dir->size].inode = child_ino;
    dir->size++;

    inode_table[child_ino].link_count++;
    dirty = 1;
    return 0;
}

static int dir_remove_entry(uint32_t dir_ino, const char *name) {
    vfs_inode_t *dir = &inode_table[dir_ino];
    if (dir->type != VFS_TYPE_DIR) return -1;

    vfs_dirent_t *entries = (vfs_dirent_t *)dir->data;
    for (uint32_t i = 0; i < dir->size; i++) {
        if (strcmp(entries[i].name, name) == 0) {
            uint32_t child = entries[i].inode;
            inode_table[child].link_count--;

            /* Swap with last entry (O(1) removal) */
            if (i < dir->size - 1)
                entries[i] = entries[dir->size - 1];
            dir->size--;
            dirty = 1;
            return 0;
        }
    }
    return -1;
}

/* Update an existing ".." entry to point to a new parent */
static void dir_update_dotdot(uint32_t dir_ino, uint32_t new_parent) {
    vfs_inode_t *dir = &inode_table[dir_ino];
    vfs_dirent_t *entries = (vfs_dirent_t *)dir->data;
    for (uint32_t i = 0; i < dir->size; i++) {
        if (strcmp(entries[i].name, "..") == 0) {
            /* Adjust link counts */
            inode_table[entries[i].inode].link_count--;
            entries[i].inode = new_parent;
            inode_table[new_parent].link_count++;
            dirty = 1;
            return;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Path resolution                                                   */
/* ------------------------------------------------------------------ */

/* Extract the next path component, advancing *pp past it.
   Returns 1 if a component was extracted into comp, 0 at end. */
static int path_next_component(const char **pp, char *comp) {
    const char *p = *pp;

    /* Skip slashes */
    while (*p == '/') p++;
    if (*p == '\0') { *pp = p; return 0; }

    /* Copy component */
    int len = 0;
    while (*p && *p != '/' && len < VFS_MAX_NAME) {
        comp[len++] = *p++;
    }
    comp[len] = '\0';

    /* Skip trailing chars if name was truncated */
    while (*p && *p != '/') p++;

    *pp = p;
    return 1;
}

int32_t vfs_resolve(const char *path, uint32_t *parent_out, char *leaf_out) {
    if (!path) return -1;

    /* Starting inode: root if absolute, cwd if relative */
    uint32_t cur = (path[0] == '/') ? 0 : effective_cwd();

    /* Handle empty path or bare "/" */
    const char *p = path;
    while (*p == '/') p++;
    if (*p == '\0') {
        /* Path is empty or just slashes → resolve to starting point */
        if (parent_out) {
            /* For bare "/", parent is root and leaf is empty */
            *parent_out = cur;
            if (leaf_out) leaf_out[0] = '\0';
        }
        return (int32_t)cur;
    }

    char comp[VFS_MAX_NAME + 1];
    char next_comp[VFS_MAX_NAME + 1];

    /* Reset pointer */
    p = path;
    if (*p == '/') {
        cur = 0;
        p++;
    }

    /* Extract the first component */
    if (!path_next_component(&p, comp))
        return (int32_t)cur;

    while (1) {
        /* Peek ahead: is there another component? */
        int has_next = path_next_component(&p, next_comp);

        if (!has_next) {
            /* comp is the last component */
            if (parent_out) {
                /* Caller wants parent + leaf, don't resolve the last component */
                *parent_out = cur;
                if (leaf_out)
                    strcpy(leaf_out, comp);
                /* Still return the resolved inode if it exists */
                return dir_lookup(cur, comp);
            }
            /* Resolve the last component fully */
            int32_t ino = dir_lookup(cur, comp);
            return ino;
        }

        /* comp is not the last component — must resolve it as a directory */
        int32_t ino = dir_lookup(cur, comp);
        if (ino < 0) return -1;
        if (inode_table[ino].type != VFS_TYPE_DIR) return -1;

        cur = (uint32_t)ino;
        strcpy(comp, next_comp);
    }
}

/* ------------------------------------------------------------------ */
/*  Initialization                                                    */
/* ------------------------------------------------------------------ */

void vfs_init(uint32_t max_inodes) {
    if (max_inodes > VFS_MAX_INODES_CAP)
        max_inodes = VFS_MAX_INODES_CAP;
    if (max_inodes < 256)
        max_inodes = 256;

    num_inodes = max_inodes;
    inode_table = (vfs_inode_t *)kcalloc(num_inodes, sizeof(vfs_inode_t));
    if (!inode_table) {
        printf("[vfs] FATAL: cannot allocate inode table (%d inodes)\n", num_inodes);
        return;
    }

    /* Set up root directory (inode 0) */
    inode_table[0].type = VFS_TYPE_DIR;
    inode_table[0].link_count = 0;
    inode_table[0].size = 0;
    inode_table[0].capacity = 0;
    inode_table[0].data = NULL;

    dir_add_entry(0, ".", 0);
    dir_add_entry(0, "..", 0);

    cwd_inode = 0;
    dirty = 0;

    printf("[vfs] initialized (%d inodes)\n", num_inodes);
}

void vfs_import_initrd(void) {
    uint32_t count = initrd_count();
    if (count == 0) return;

    uint32_t imported = 0;
    for (uint32_t i = 0; i < count; i++) {
        const char *name;
        uint32_t phys, size;

        if (initrd_get_entry(i, &name, &phys, &size) != 0)
            continue;

        /* Allocate a new file inode */
        int32_t ino = inode_alloc(VFS_TYPE_FILE);
        if (ino < 0) break;

        /* Copy file data from initrd physical pages into a heap buffer */
        vfs_inode_t *node = &inode_table[ino];
        if (size > 0) {
            node->data = kmalloc(size);
            if (!node->data) {
                inode_free((uint32_t)ino);
                continue;
            }
            node->size = size;
            node->capacity = size;

            /* Copy page-by-page using temp_map */
            uint8_t *dst = (uint8_t *)node->data;
            uint32_t remaining = size;
            uint32_t src_phys = phys;

            while (remaining > 0) {
                uint32_t page_base = src_phys & ~0xFFFu;
                uint32_t page_off  = src_phys & 0xFFFu;
                uint32_t chunk = 0x1000 - page_off;
                if (chunk > remaining) chunk = remaining;

                uint8_t *mapped = (uint8_t *)temp_map(page_base);
                memcpy(dst, mapped + page_off, chunk);
                temp_unmap();

                dst       += chunk;
                src_phys  += chunk;
                remaining -= chunk;
            }
        }

        /* Add to root directory */
        if (dir_add_entry(0, name, (uint32_t)ino) != 0) {
            inode_free((uint32_t)ino);
            continue;
        }

        imported++;
    }

    if (imported > 0)
        printf("[vfs] imported %d file(s) from initrd\n", imported);
}

/* ------------------------------------------------------------------ */
/*  File/directory operations                                         */
/* ------------------------------------------------------------------ */

int32_t vfs_create_file(const char *path) {
    uint32_t parent_ino;
    char leaf[VFS_MAX_NAME + 1];

    vfs_resolve(path, &parent_ino, leaf);

    if (leaf[0] == '\0') {
        printf("[vfs] create_file: empty name\n");
        return -1;
    }
    if (inode_table[parent_ino].type != VFS_TYPE_DIR) {
        printf("[vfs] create_file: parent not a directory\n");
        return -1;
    }
    if (dir_lookup(parent_ino, leaf) >= 0) {
        printf("[vfs] create_file: '%s' already exists\n", leaf);
        return -1;
    }

    int32_t ino = inode_alloc(VFS_TYPE_FILE);
    if (ino < 0) return -1;

    if (dir_add_entry(parent_ino, leaf, (uint32_t)ino) != 0) {
        inode_free((uint32_t)ino);
        return -1;
    }

    return ino;
}

int32_t vfs_mkdir(const char *path) {
    uint32_t parent_ino;
    char leaf[VFS_MAX_NAME + 1];

    vfs_resolve(path, &parent_ino, leaf);

    if (leaf[0] == '\0') {
        printf("[vfs] mkdir: empty name\n");
        return -1;
    }
    if (inode_table[parent_ino].type != VFS_TYPE_DIR) {
        printf("[vfs] mkdir: parent not a directory\n");
        return -1;
    }
    if (dir_lookup(parent_ino, leaf) >= 0) {
        printf("[vfs] mkdir: '%s' already exists\n", leaf);
        return -1;
    }

    int32_t ino = inode_alloc(VFS_TYPE_DIR);
    if (ino < 0) return -1;

    /* Add "." and ".." entries to the new directory */
    dir_add_entry((uint32_t)ino, ".", (uint32_t)ino);
    dir_add_entry((uint32_t)ino, "..", parent_ino);

    /* Add entry in parent */
    if (dir_add_entry(parent_ino, leaf, (uint32_t)ino) != 0) {
        inode_free((uint32_t)ino);
        return -1;
    }

    return ino;
}

int vfs_remove(const char *path) {
    uint32_t parent_ino;
    char leaf[VFS_MAX_NAME + 1];

    int32_t ino = vfs_resolve(path, &parent_ino, leaf);
    if (ino < 0) {
        printf("[vfs] rm: not found\n");
        return -1;
    }

    /* Cannot remove root */
    if ((uint32_t)ino == 0) {
        printf("[vfs] rm: cannot remove root\n");
        return -1;
    }

    /* Cannot remove "." or ".." */
    if (strcmp(leaf, ".") == 0 || strcmp(leaf, "..") == 0) {
        printf("[vfs] rm: cannot remove '.' or '..'\n");
        return -1;
    }

    /* Cannot remove cwd */
    if ((uint32_t)ino == effective_cwd()) {
        printf("[vfs] rm: cannot remove current directory\n");
        return -1;
    }

    vfs_inode_t *node = &inode_table[ino];

    if (node->type == VFS_TYPE_DIR) {
        /* Only remove empty directories (only "." and "..") */
        if (node->size > 2) {
            printf("[vfs] rm: directory not empty\n");
            return -1;
        }
        /* Decrement parent's link count for removed ".." */
        inode_table[parent_ino].link_count--;
    }

    /* Remove entry from parent */
    dir_remove_entry(parent_ino, leaf);

    /* Free inode if no more links */
    if (node->link_count == 0)
        inode_free((uint32_t)ino);

    return 0;
}

int32_t vfs_read(uint32_t ino, void *buf, uint32_t offset, uint32_t count) {
    if (ino >= num_inodes) return -1;
    vfs_inode_t *node = &inode_table[ino];
    if (node->type != VFS_TYPE_FILE) return -1;

    if (offset >= node->size) return 0;
    if (offset + count > node->size)
        count = node->size - offset;

    memcpy(buf, (uint8_t *)node->data + offset, count);
    return (int32_t)count;
}

int32_t vfs_write(uint32_t ino, const void *buf, uint32_t offset, uint32_t count) {
    if (ino >= num_inodes) return -1;
    vfs_inode_t *node = &inode_table[ino];
    if (node->type != VFS_TYPE_FILE) return -1;

    uint32_t end = offset + count;

    /* Grow buffer if needed */
    if (end > node->capacity) {
        uint32_t new_cap = node->capacity ? node->capacity : 64;
        while (new_cap < end) new_cap *= 2;

        void *new_data = krealloc(node->data, new_cap);
        if (!new_data) {
            printf("[vfs] write: out of memory\n");
            return -1;
        }
        /* Zero new region between old size and offset */
        if (offset > node->size)
            memset((uint8_t *)new_data + node->size, 0, offset - node->size);
        node->data = new_data;
        node->capacity = new_cap;
    }

    memcpy((uint8_t *)node->data + offset, buf, count);
    if (end > node->size)
        node->size = end;

    dirty = 1;
    return (int32_t)count;
}

int vfs_rename(const char *old_path, const char *new_path) {
    uint32_t old_parent;
    char old_leaf[VFS_MAX_NAME + 1];
    int32_t ino = vfs_resolve(old_path, &old_parent, old_leaf);
    if (ino < 0) {
        printf("[vfs] rename: source not found\n");
        return -1;
    }
    if ((uint32_t)ino == 0) {
        printf("[vfs] rename: cannot rename root\n");
        return -1;
    }

    uint32_t new_parent;
    char new_leaf[VFS_MAX_NAME + 1];
    vfs_resolve(new_path, &new_parent, new_leaf);

    if (new_leaf[0] == '\0') {
        printf("[vfs] rename: invalid destination\n");
        return -1;
    }
    if (inode_table[new_parent].type != VFS_TYPE_DIR) {
        printf("[vfs] rename: destination parent not a directory\n");
        return -1;
    }

    /* If destination exists, remove it first (files only) */
    int32_t existing = dir_lookup(new_parent, new_leaf);
    if (existing >= 0) {
        if (inode_table[existing].type == VFS_TYPE_DIR) {
            printf("[vfs] rename: cannot overwrite directory\n");
            return -1;
        }
        dir_remove_entry(new_parent, new_leaf);
        if (inode_table[existing].link_count == 0)
            inode_free((uint32_t)existing);
    }

    /* Remove from old parent, add to new parent */
    dir_remove_entry(old_parent, old_leaf);
    dir_add_entry(new_parent, new_leaf, (uint32_t)ino);

    /* Update ".." if moving a directory to a new parent */
    if (inode_table[ino].type == VFS_TYPE_DIR && old_parent != new_parent)
        dir_update_dotdot((uint32_t)ino, new_parent);

    return 0;
}

int32_t vfs_copy(const char *src_path, const char *dst_path) {
    int32_t src_ino = vfs_resolve(src_path, NULL, NULL);
    if (src_ino < 0) {
        printf("[vfs] copy: source not found\n");
        return -1;
    }
    if (inode_table[src_ino].type != VFS_TYPE_FILE) {
        printf("[vfs] copy: can only copy files\n");
        return -1;
    }

    uint32_t dst_parent;
    char dst_leaf[VFS_MAX_NAME + 1];
    vfs_resolve(dst_path, &dst_parent, dst_leaf);

    if (dst_leaf[0] == '\0') {
        printf("[vfs] copy: invalid destination\n");
        return -1;
    }
    if (inode_table[dst_parent].type != VFS_TYPE_DIR) {
        printf("[vfs] copy: destination parent not a directory\n");
        return -1;
    }

    /* If destination already exists, fail */
    if (dir_lookup(dst_parent, dst_leaf) >= 0) {
        printf("[vfs] copy: '%s' already exists\n", dst_leaf);
        return -1;
    }

    /* Allocate new file inode */
    int32_t new_ino = inode_alloc(VFS_TYPE_FILE);
    if (new_ino < 0) return -1;

    /* Copy data */
    vfs_inode_t *src = &inode_table[src_ino];
    vfs_inode_t *dst = &inode_table[new_ino];

    if (src->size > 0) {
        dst->data = kmalloc(src->size);
        if (!dst->data) {
            inode_free((uint32_t)new_ino);
            return -1;
        }
        memcpy(dst->data, src->data, src->size);
        dst->size = src->size;
        dst->capacity = src->size;
    }

    /* Add to destination directory */
    if (dir_add_entry(dst_parent, dst_leaf, (uint32_t)new_ino) != 0) {
        inode_free((uint32_t)new_ino);
        return -1;
    }

    return new_ino;
}

/* ------------------------------------------------------------------ */
/*  Directory listing                                                 */
/* ------------------------------------------------------------------ */

int vfs_list(uint32_t dir_ino) {
    if (dir_ino >= num_inodes) return -1;
    vfs_inode_t *dir = &inode_table[dir_ino];
    if (dir->type != VFS_TYPE_DIR) {
        printf("[vfs] ls: not a directory\n");
        return -1;
    }

    vfs_dirent_t *entries = (vfs_dirent_t *)dir->data;
    for (uint32_t i = 0; i < dir->size; i++) {
        vfs_inode_t *child = &inode_table[entries[i].inode];
        if (child->type == VFS_TYPE_DIR) {
            printf("  %s/\n", entries[i].name);
        } else {
            printf("  %s  (%d bytes)\n", entries[i].name, child->size);
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Current working directory                                         */
/* ------------------------------------------------------------------ */

uint32_t vfs_get_cwd(void) {
    return effective_cwd();
}

int vfs_chdir(const char *path) {
    int32_t ino = vfs_resolve(path, NULL, NULL);
    if (ino < 0) return -1;
    if (inode_table[ino].type != VFS_TYPE_DIR) return -1;

    if (current_process)
        current_process->cwd = (uint32_t)ino;
    else
        cwd_inode = (uint32_t)ino;
    return 0;
}

const char *vfs_get_cwd_path(void) {
    uint32_t cwd = effective_cwd();
    if (cwd == 0) return "/";

    /* Build path by walking ".." to root */
    /* Collect components in reverse, then reverse */
    char components[16][VFS_MAX_NAME + 1];
    int depth = 0;
    uint32_t cur = cwd;

    while (cur != 0 && depth < 16) {
        /* Find parent */
        int32_t parent = dir_lookup(cur, "..");
        if (parent < 0 || (uint32_t)parent == cur) break;  /* at root */

        /* Find our name in the parent directory */
        vfs_inode_t *pdir = &inode_table[parent];
        vfs_dirent_t *entries = (vfs_dirent_t *)pdir->data;
        const char *name = "?";
        for (uint32_t i = 0; i < pdir->size; i++) {
            if (entries[i].inode == cur &&
                strcmp(entries[i].name, ".") != 0 &&
                strcmp(entries[i].name, "..") != 0) {
                name = entries[i].name;
                break;
            }
        }

        strcpy(components[depth], name);
        depth++;
        cur = (uint32_t)parent;
    }

    /* Build the path string */
    cwd_path_buf[0] = '\0';
    int pos = 0;
    for (int i = depth - 1; i >= 0 && pos < VFS_PATH_MAX - 2; i--) {
        cwd_path_buf[pos++] = '/';
        int len = (int)strlen(components[i]);
        if (pos + len >= VFS_PATH_MAX - 1) break;
        strcpy(cwd_path_buf + pos, components[i]);
        pos += len;
    }
    cwd_path_buf[pos] = '\0';

    if (cwd_path_buf[0] == '\0')
        return "/";

    return cwd_path_buf;
}

/* ------------------------------------------------------------------ */
/*  Reset (for reloading from disk)                                   */
/* ------------------------------------------------------------------ */

void vfs_reset(void) {
    /* Free all inode data */
    for (uint32_t i = 0; i < num_inodes; i++) {
        if (inode_table[i].data)
            kfree(inode_table[i].data);
    }
    memset(inode_table, 0, num_inodes * sizeof(vfs_inode_t));

    /* Re-create root directory with "." and ".." */
    inode_table[0].type = VFS_TYPE_DIR;
    inode_table[0].link_count = 0;
    inode_table[0].size = 0;
    inode_table[0].capacity = 0;
    inode_table[0].data = NULL;

    dir_add_entry(0, ".", 0);
    dir_add_entry(0, "..", 0);

    cwd_inode = 0;
    if (current_process)
        current_process->cwd = 0;
    dirty = 0;
}

/* ------------------------------------------------------------------ */
/*  Inode access                                                      */
/* ------------------------------------------------------------------ */

vfs_inode_t *vfs_get_inode(uint32_t ino) {
    if (ino >= num_inodes) return (vfs_inode_t *)0;
    return &inode_table[ino];
}

uint32_t vfs_get_max_inodes(void) {
    return num_inodes;
}

int vfs_ensure_capacity(uint32_t min_inodes) {
    if (min_inodes <= num_inodes) return 0;
    if (min_inodes > VFS_MAX_INODES_CAP) min_inodes = VFS_MAX_INODES_CAP;
    if (num_inodes >= VFS_MAX_INODES_CAP) return -1;

    vfs_inode_t *new_table = (vfs_inode_t *)krealloc(
        inode_table, min_inodes * sizeof(vfs_inode_t));
    if (!new_table) {
        printf("[vfs] ensure_capacity: out of memory (%d -> %d)\n",
               num_inodes, min_inodes);
        return -1;
    }
    /* Zero the new region */
    memset(&new_table[num_inodes], 0,
           (min_inodes - num_inodes) * sizeof(vfs_inode_t));
    inode_table = new_table;
    num_inodes = min_inodes;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Dirty tracking                                                    */
/* ------------------------------------------------------------------ */

int vfs_is_dirty(void) {
    return dirty;
}

void vfs_mark_clean(void) {
    dirty = 0;
}
