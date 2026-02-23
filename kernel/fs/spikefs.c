#include <kernel/spikefs.h>
#include <kernel/ata.h>
#include <kernel/vfs.h>
#include <kernel/heap.h>
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/*  Cached disk layout (populated from superblock or format)          */
/* ------------------------------------------------------------------ */

static struct {
    uint32_t num_blocks;
    uint32_t bitmap_start;
    uint32_t data_start;
    uint32_t bitmap_sectors;
    uint32_t imap_block;      /* block# of first inode map block */
    uint32_t num_ichunks;     /* active inode chunks */
} layout;

/* ------------------------------------------------------------------ */
/*  Block bitmap (heap-allocated, sized from layout)                  */
/* ------------------------------------------------------------------ */

static uint8_t *block_bitmap = NULL;
static uint32_t bitmap_bytes = 0;

static void bitmap_clear_all(void) {
    if (block_bitmap)
        memset(block_bitmap, 0, bitmap_bytes);
}

static void bitmap_set(uint32_t blk) {
    if (blk / 8 < bitmap_bytes)
        block_bitmap[blk / 8] |= (1 << (blk % 8));
}

static int bitmap_test(uint32_t blk) {
    if (blk / 8 >= bitmap_bytes) return 1;  /* out of range = occupied */
    return (block_bitmap[blk / 8] >> (blk % 8)) & 1;
}

/* Find 'count' consecutive free blocks. Returns first block index, or -1. */
static int32_t bitmap_alloc(uint32_t count) {
    uint32_t run = 0;
    uint32_t start = 0;

    for (uint32_t i = 0; i < layout.num_blocks; i++) {
        if (!bitmap_test(i)) {
            if (run == 0) start = i;
            run++;
            if (run == count) {
                for (uint32_t j = start; j < start + count; j++)
                    bitmap_set(j);
                return (int32_t)start;
            }
        } else {
            run = 0;
        }
    }
    return -1;
}

/* Allocate or resize the block bitmap */
static int bitmap_init(uint32_t sectors) {
    uint32_t bytes = sectors * 512;
    if (block_bitmap && bitmap_bytes == bytes) {
        return 0;
    }
    if (block_bitmap)
        kfree(block_bitmap);

    block_bitmap = (uint8_t *)kmalloc(bytes);
    if (!block_bitmap) {
        printf("[spikefs] out of memory for bitmap (%d bytes)\n", bytes);
        bitmap_bytes = 0;
        return -1;
    }
    bitmap_bytes = bytes;
    memset(block_bitmap, 0, bytes);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Sector I/O helpers                                                */
/* ------------------------------------------------------------------ */

static uint8_t sector_buf[512] __attribute__((aligned(4)));

static int read_sector(uint32_t lba, void *buf) {
    return ata_read_sectors(lba, 1, buf);
}

static int write_sector(uint32_t lba, const void *buf) {
    return ata_write_sectors(lba, 1, buf);
}

static int read_sectors(uint32_t lba, uint32_t count, void *buf) {
    uint8_t *p = (uint8_t *)buf;
    for (uint32_t i = 0; i < count; i++) {
        if (read_sector(lba + i, p + i * 512) != 0)
            return -1;
    }
    return 0;
}

static int write_sectors(uint32_t lba, uint32_t count, const void *buf) {
    const uint8_t *p = (const uint8_t *)buf;
    for (uint32_t i = 0; i < count; i++) {
        if (write_sector(lba + i, p + i * 512) != 0)
            return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Layout calculation (v3: no inode region, just bitmap + data pool) */
/* ------------------------------------------------------------------ */

static void calculate_layout(uint32_t total_sectors) {
    layout.bitmap_start = 1;

    uint32_t data_approx = total_sectors - 1;
    layout.bitmap_sectors = (data_approx + 4095) / 4096;

    layout.data_start = 1 + layout.bitmap_sectors;
    layout.num_blocks = total_sectors - layout.data_start;
}

static void populate_layout_from_super(spikefs_super_t *super) {
    layout.num_blocks     = super->num_blocks;
    layout.bitmap_start   = super->bitmap_start;
    layout.data_start     = super->data_start;
    layout.imap_block     = super->imap_block;
    layout.num_ichunks    = super->num_ichunks;
    layout.bitmap_sectors = layout.data_start - layout.bitmap_start;
}

/* ------------------------------------------------------------------ */
/*  Format (v3: inode chunks in data pool)                            */
/* ------------------------------------------------------------------ */

int spikefs_format(void) {
    if (!ata_present()) return -1;

    uint32_t total = ata_total_sectors();
    if (total < 64) {
        printf("[spikefs] disk too small (%d sectors)\n", total);
        return -1;
    }

    calculate_layout(total);

    /* Allocate bitmap */
    if (bitmap_init(layout.bitmap_sectors) != 0)
        return -1;
    bitmap_clear_all();

    /* Block 0: inode chunk 0 (holds root inode + 7 free slots) */
    bitmap_set(0);
    /* Block 1: inode map block */
    bitmap_set(1);

    layout.imap_block = 1;
    layout.num_ichunks = 1;

    /* Build inode chunk 0: root directory inode at slot 0 */
    uint8_t chunk_buf[512];
    memset(chunk_buf, 0, 512);
    spikefs_inode_t *root_inode = (spikefs_inode_t *)chunk_buf;
    root_inode->type = VFS_TYPE_DIR;
    root_inode->link_count = 2;  /* "." and ".." */
    root_inode->size = 0;        /* empty dir, data written by first sync */

    if (write_sector(layout.data_start + 0, chunk_buf) != 0) return -1;

    /* Build inode map block: entry[0]=0 (chunk 0 at block 0) */
    uint32_t imap_buf[128];
    memset(imap_buf, 0, sizeof(imap_buf));
    imap_buf[0] = 0;  /* chunk 0 is at block 0 */
    /* entry[127] = 0 (no next imap block) */

    if (write_sector(layout.data_start + 1, imap_buf) != 0) return -1;

    /* Write zeroed bitmap sectors */
    /* First, write the actual bitmap with blocks 0,1 marked */
    if (write_sectors(layout.bitmap_start, layout.bitmap_sectors,
                      block_bitmap) != 0)
        return -1;

    /* Write superblock */
    spikefs_super_t super;
    memset(&super, 0, sizeof(super));
    super.magic        = SPIKEFS_MAGIC;
    super.version      = SPIKEFS_VERSION;
    super.num_blocks   = layout.num_blocks;
    super.bitmap_start = layout.bitmap_start;
    super.data_start   = layout.data_start;
    super.imap_block   = layout.imap_block;
    super.num_ichunks  = layout.num_ichunks;

    if (write_sector(0, &super) != 0) return -1;

    ata_flush();

    printf("[spikefs] formatted: %d data blocks, 1 inode chunk\n",
           layout.num_blocks);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Sync (VFS -> disk) — v3 chunk-based write-back                    */
/* ------------------------------------------------------------------ */

int spikefs_sync(void) {
    if (!ata_present()) return -1;

    uint32_t vfs_count = vfs_get_max_inodes();

    /* 1. Find highest used inode to determine chunks needed */
    uint32_t highest = 0;
    for (uint32_t i = 0; i < vfs_count; i++) {
        vfs_inode_t *vnode = vfs_get_inode(i);
        if (vnode && vnode->type != VFS_TYPE_FREE)
            highest = i;
    }
    uint32_t num_ichunks = (highest / SPIKEFS_ICHUNK_INODES) + 1;

    /* 2. Calculate imap blocks needed */
    uint32_t num_imap_blocks = (num_ichunks + SPIKEFS_IMAP_ENTRIES - 1)
                               / SPIKEFS_IMAP_ENTRIES;

    /* 3. Clear bitmap and allocate chunk blocks + imap blocks */
    bitmap_clear_all();

    /* Allocate inode chunks (consecutive) */
    int32_t chunk_start = bitmap_alloc(num_ichunks);
    if (chunk_start < 0) {
        printf("[spikefs] sync: out of space for %d inode chunks\n",
               num_ichunks);
        return -1;
    }

    /* Allocate imap blocks (consecutive) */
    int32_t imap_start = bitmap_alloc(num_imap_blocks);
    if (imap_start < 0) {
        printf("[spikefs] sync: out of space for imap\n");
        return -1;
    }

    layout.imap_block = (uint32_t)imap_start;
    layout.num_ichunks = num_ichunks;

    /* 4. Build and write inode chunks, allocating data blocks per inode */
    for (uint32_t c = 0; c < num_ichunks; c++) {
        uint8_t chunk_buf_local[512];
        memset(chunk_buf_local, 0, 512);
        spikefs_inode_t *disk_inodes = (spikefs_inode_t *)chunk_buf_local;

        for (uint32_t j = 0; j < SPIKEFS_ICHUNK_INODES; j++) {
            uint32_t ino = c * SPIKEFS_ICHUNK_INODES + j;
            if (ino >= vfs_count) break;

            vfs_inode_t *vnode = vfs_get_inode(ino);
            if (!vnode || vnode->type == VFS_TYPE_FREE) {
                memset(&disk_inodes[j], 0, sizeof(spikefs_inode_t));
                continue;
            }

            disk_inodes[j].type = vnode->type;
            disk_inodes[j].link_count = vnode->link_count;
            disk_inodes[j].indirect = 0;
            disk_inodes[j].reserved = 0;

            /* Determine data bytes */
            uint32_t data_bytes;
            if (vnode->type == VFS_TYPE_FILE) {
                data_bytes = vnode->size;
            } else {
                data_bytes = vnode->size * sizeof(vfs_dirent_t);
            }
            disk_inodes[j].size = data_bytes;

            if (data_bytes == 0) {
                memset(disk_inodes[j].direct, 0,
                       sizeof(disk_inodes[j].direct));
                continue;
            }

            /* Allocate data blocks */
            uint32_t blocks_needed = (data_bytes + 511) / 512;
            uint32_t direct_count = blocks_needed;
            uint32_t indirect_count = 0;

            if (blocks_needed > SPIKEFS_DIRECT_BLOCKS) {
                direct_count = SPIKEFS_DIRECT_BLOCKS;
                indirect_count = blocks_needed - SPIKEFS_DIRECT_BLOCKS;
            }

            uint32_t total_alloc = blocks_needed
                                   + (indirect_count > 0 ? 1 : 0);
            int32_t first_block = bitmap_alloc(total_alloc);
            if (first_block < 0) {
                printf("[spikefs] sync: out of space for inode %d\n", ino);
                return -1;
            }

            /* Fill direct block pointers */
            for (uint32_t d = 0; d < SPIKEFS_DIRECT_BLOCKS; d++) {
                if (d < direct_count)
                    disk_inodes[j].direct[d] =
                        (uint32_t)first_block + d;
                else
                    disk_inodes[j].direct[d] = 0;
            }

            /* Write data to direct blocks */
            uint8_t *data_ptr = (uint8_t *)vnode->data;
            uint32_t remaining = data_bytes;

            for (uint32_t d = 0; d < direct_count && remaining > 0; d++) {
                uint32_t sector = layout.data_start
                                  + (uint32_t)first_block + d;
                uint32_t chunk = remaining > 512 ? 512 : remaining;

                memset(sector_buf, 0, 512);
                memcpy(sector_buf, data_ptr, chunk);
                if (write_sector(sector, sector_buf) != 0)
                    return -1;

                data_ptr += chunk;
                remaining -= chunk;
            }

            /* Handle indirect blocks if needed */
            if (indirect_count > 0) {
                uint32_t indirect_blk =
                    (uint32_t)first_block + direct_count;
                disk_inodes[j].indirect = indirect_blk;

                uint32_t indirect_entries[128];
                memset(indirect_entries, 0, sizeof(indirect_entries));

                for (uint32_t k = 0;
                     k < indirect_count && k < 128;
                     k++) {
                    uint32_t data_blk = indirect_blk + 1 + k;
                    indirect_entries[k] = data_blk;

                    uint32_t sector = layout.data_start + data_blk;
                    uint32_t chunk = remaining > 512 ? 512 : remaining;

                    memset(sector_buf, 0, 512);
                    memcpy(sector_buf, data_ptr, chunk);
                    if (write_sector(sector, sector_buf) != 0)
                        return -1;

                    data_ptr += chunk;
                    remaining -= chunk;
                }

                uint32_t sector = layout.data_start + indirect_blk;
                if (write_sector(sector, indirect_entries) != 0)
                    return -1;
            }
        }

        /* Write this inode chunk to disk */
        uint32_t chunk_sector = layout.data_start
                                + (uint32_t)chunk_start + c;
        if (write_sector(chunk_sector, chunk_buf_local) != 0)
            return -1;
    }

    /* 5. Build and write inode map blocks */
    uint32_t chunks_written = 0;
    for (uint32_t m = 0; m < num_imap_blocks; m++) {
        uint32_t imap_buf[128];
        memset(imap_buf, 0, sizeof(imap_buf));

        uint32_t entries_this = num_ichunks - chunks_written;
        if (entries_this > SPIKEFS_IMAP_ENTRIES)
            entries_this = SPIKEFS_IMAP_ENTRIES;

        for (uint32_t e = 0; e < entries_this; e++) {
            /* Chunk block number = chunk_start + chunk_index */
            imap_buf[e] = (uint32_t)chunk_start + chunks_written + e;
        }

        /* Entry 127 = next imap block (0 if last) */
        if (m + 1 < num_imap_blocks) {
            imap_buf[127] = (uint32_t)imap_start + m + 1;
        } else {
            imap_buf[127] = 0;
        }

        uint32_t imap_sector = layout.data_start
                               + (uint32_t)imap_start + m;
        if (write_sector(imap_sector, imap_buf) != 0)
            return -1;

        chunks_written += entries_this;
    }

    /* 6. Write bitmap */
    if (write_sectors(layout.bitmap_start, layout.bitmap_sectors,
                      block_bitmap) != 0)
        return -1;

    /* 7. Write superblock */
    spikefs_super_t super;
    memset(&super, 0, sizeof(super));
    super.magic        = SPIKEFS_MAGIC;
    super.version      = SPIKEFS_VERSION;
    super.num_blocks   = layout.num_blocks;
    super.bitmap_start = layout.bitmap_start;
    super.data_start   = layout.data_start;
    super.imap_block   = layout.imap_block;
    super.num_ichunks  = layout.num_ichunks;

    if (write_sector(0, &super) != 0)
        return -1;

    /* 8. Flush and clear dirty */
    ata_flush();
    vfs_mark_clean();

    printf("[spikefs] synced to disk (%d inode chunks)\n", num_ichunks);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Load (disk -> VFS) — v3 chunk-based                               */
/* ------------------------------------------------------------------ */

int spikefs_load(void) {
    if (!ata_present()) return -1;

    /* Allocate bitmap */
    if (bitmap_init(layout.bitmap_sectors) != 0)
        return -1;

    /* Read bitmap from disk */
    if (read_sectors(layout.bitmap_start, layout.bitmap_sectors,
                     block_bitmap) != 0) {
        printf("[spikefs] load: failed to read bitmap\n");
        return -1;
    }

    /* Read inode map chain to get chunk block numbers */
    uint32_t total_inodes = layout.num_ichunks * SPIKEFS_ICHUNK_INODES;

    /* Ensure VFS can hold all inodes */
    if (vfs_ensure_capacity(total_inodes) != 0) {
        printf("[spikefs] load: cannot grow VFS to %d inodes\n",
               total_inodes);
        return -1;
    }

    /* Collect all chunk block numbers by walking the imap chain */
    uint32_t *chunk_blocks = (uint32_t *)kmalloc(
        layout.num_ichunks * sizeof(uint32_t));
    if (!chunk_blocks) {
        printf("[spikefs] load: out of memory for chunk list\n");
        return -1;
    }

    uint32_t chunks_read = 0;
    uint32_t imap_blk = layout.imap_block;

    while (imap_blk != 0 && chunks_read < layout.num_ichunks) {
        uint32_t imap_buf[128];
        uint32_t imap_sector = layout.data_start + imap_blk;

        if (read_sector(imap_sector, imap_buf) != 0) {
            kfree(chunk_blocks);
            printf("[spikefs] load: failed to read imap block\n");
            return -1;
        }

        uint32_t entries = layout.num_ichunks - chunks_read;
        if (entries > SPIKEFS_IMAP_ENTRIES)
            entries = SPIKEFS_IMAP_ENTRIES;

        for (uint32_t e = 0; e < entries; e++) {
            chunk_blocks[chunks_read + e] = imap_buf[e];
        }

        chunks_read += entries;
        imap_blk = imap_buf[127];  /* next imap block (0 = end) */
    }

    /* Reset in-memory VFS */
    vfs_reset();

    /* Read each inode chunk and populate VFS */
    for (uint32_t c = 0; c < layout.num_ichunks; c++) {
        uint8_t chunk_buf_local[512];
        uint32_t chunk_sector = layout.data_start + chunk_blocks[c];

        if (read_sector(chunk_sector, chunk_buf_local) != 0) {
            kfree(chunk_blocks);
            printf("[spikefs] load: failed to read chunk %d\n", c);
            return -1;
        }

        spikefs_inode_t *disk_inodes =
            (spikefs_inode_t *)chunk_buf_local;

        for (uint32_t j = 0; j < SPIKEFS_ICHUNK_INODES; j++) {
            uint32_t ino = c * SPIKEFS_ICHUNK_INODES + j;

            if (disk_inodes[j].type == VFS_TYPE_FREE)
                continue;

            vfs_inode_t *vnode = vfs_get_inode(ino);
            if (!vnode) continue;

            vnode->type = disk_inodes[j].type;
            vnode->link_count = disk_inodes[j].link_count;

            uint32_t data_bytes = disk_inodes[j].size;

            if (data_bytes == 0) {
                if (vnode->type == VFS_TYPE_DIR) {
                    vnode->size = 0;
                    vnode->capacity = 0;
                    vnode->data = NULL;
                }
                continue;
            }

            /* Allocate memory for data */
            void *data = kmalloc(data_bytes);
            if (!data) {
                printf("[spikefs] load: out of memory for inode %d\n",
                       ino);
                kfree(chunk_blocks);
                return -1;
            }

            /* Read data from disk blocks */
            uint8_t *dst = (uint8_t *)data;
            uint32_t remaining = data_bytes;

            uint32_t blocks_needed = (data_bytes + 511) / 512;
            uint32_t direct_count = blocks_needed;
            if (direct_count > SPIKEFS_DIRECT_BLOCKS)
                direct_count = SPIKEFS_DIRECT_BLOCKS;

            for (uint32_t d = 0;
                 d < direct_count && remaining > 0;
                 d++) {
                uint32_t blk = disk_inodes[j].direct[d];
                uint32_t sector = layout.data_start + blk;

                if (read_sector(sector, sector_buf) != 0) {
                    kfree(data);
                    kfree(chunk_blocks);
                    return -1;
                }

                uint32_t chunk = remaining > 512 ? 512 : remaining;
                memcpy(dst, sector_buf, chunk);
                dst += chunk;
                remaining -= chunk;
            }

            /* Read from indirect blocks if needed */
            if (remaining > 0 && disk_inodes[j].indirect != 0) {
                uint32_t indirect_entries[128];
                uint32_t sector = layout.data_start
                                  + disk_inodes[j].indirect;

                if (read_sector(sector, indirect_entries) != 0) {
                    kfree(data);
                    kfree(chunk_blocks);
                    return -1;
                }

                for (uint32_t k = 0;
                     k < 128 && remaining > 0;
                     k++) {
                    if (indirect_entries[k] == 0) break;

                    sector = layout.data_start + indirect_entries[k];
                    if (read_sector(sector, sector_buf) != 0) {
                        kfree(data);
                        kfree(chunk_blocks);
                        return -1;
                    }

                    uint32_t chunk = remaining > 512
                                     ? 512 : remaining;
                    memcpy(dst, sector_buf, chunk);
                    dst += chunk;
                    remaining -= chunk;
                }
            }

            /* Set the VFS inode fields */
            if (vnode->type == VFS_TYPE_FILE) {
                vnode->size = data_bytes;
                vnode->capacity = data_bytes;
                vnode->data = data;
            } else {
                uint32_t num_entries =
                    data_bytes / sizeof(spikefs_dirent_t);
                vnode->size = num_entries;
                vnode->capacity = num_entries;
                vnode->data = data;
            }
        }
    }

    kfree(chunk_blocks);
    vfs_mark_clean();
    printf("[spikefs] loaded from disk (%d inode chunks, %d inodes)\n",
           layout.num_ichunks,
           layout.num_ichunks * SPIKEFS_ICHUNK_INODES);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Init (called at boot)                                             */
/* ------------------------------------------------------------------ */

int spikefs_init(void) {
    if (!ata_present()) {
        printf("[spikefs] no disk, skipping\n");
        return -1;
    }

    /* Read superblock */
    spikefs_super_t super;
    if (read_sector(0, &super) != 0) {
        printf("[spikefs] failed to read superblock\n");
        return -1;
    }

    if (super.magic == SPIKEFS_MAGIC && super.version == SPIKEFS_VERSION) {
        /* Valid v3 filesystem — populate layout and load */
        populate_layout_from_super(&super);
        printf("[spikefs] found v3 filesystem (%d inode chunks), loading...\n",
               layout.num_ichunks);
        return spikefs_load();
    }

    /* Blank or incompatible disk — format and sync current VFS */
    printf("[spikefs] no valid v3 filesystem, formatting...\n");
    if (spikefs_format() != 0)
        return -1;

    return spikefs_sync();
}
