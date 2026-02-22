#ifndef _SPIKEFS_H
#define _SPIKEFS_H

#include <stdint.h>

/* ------------------------------------------------------------------ */
/*  On-disk layout constants                                          */
/* ------------------------------------------------------------------ */

#define SPIKEFS_MAGIC       0x534B4653  /* "SKFS" */
#define SPIKEFS_VERSION     3           /* v3: inode chunks (btrfs/XFS-style) */

#define SPIKEFS_DIRECT_BLOCKS 12
#define SPIKEFS_NAME_MAX      60

/* Inode chunk: 8 inodes (64 bytes each) per 512-byte block */
#define SPIKEFS_ICHUNK_INODES 8

/* Inode map: 127 chunk entries per block + 1 "next" pointer */
#define SPIKEFS_IMAP_ENTRIES  127

/* ------------------------------------------------------------------ */
/*  On-disk structures                                                */
/* ------------------------------------------------------------------ */

/*
 * Superblock (v3) — 512 bytes.
 *
 * v3 eliminates the fixed inode table region. Inodes are stored in
 * "inode chunk" blocks allocated from the unified data pool (like
 * btrfs/XFS). The inode map block tracks chunk locations.
 *
 * Layout:
 *   Sector 0:            Superblock
 *   Sectors 1..B:        Block bitmap
 *   Sectors B+1..end:    Data pool (inode chunks + file data)
 */
typedef struct spikefs_super {
    uint32_t magic;
    uint32_t version;
    uint32_t num_blocks;      /* total blocks in data pool */
    uint32_t bitmap_start;    /* first sector of block bitmap */
    uint32_t data_start;      /* first sector of data pool */
    uint32_t imap_block;      /* block# of first inode map (in data pool) */
    uint32_t num_ichunks;     /* number of active inode chunks */
    uint8_t  pad[484];
} __attribute__((packed)) spikefs_super_t;

/* 64 bytes — 8 per block (one "inode chunk") */
typedef struct spikefs_inode {
    uint8_t  type;              /* 0=free, 1=file, 2=dir */
    uint8_t  pad;
    uint16_t link_count;
    uint32_t size;              /* bytes of data */
    uint32_t direct[SPIKEFS_DIRECT_BLOCKS];  /* 12 direct block numbers */
    uint32_t indirect;          /* single indirect block number (0=none) */
    uint32_t reserved;
} __attribute__((packed)) spikefs_inode_t;

/* 64 bytes — 8 per block, matches vfs_dirent_t layout */
typedef struct spikefs_dirent {
    char     name[SPIKEFS_NAME_MAX];
    uint32_t inode;
} __attribute__((packed)) spikefs_dirent_t;

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

/* Called at boot after ata_init() + vfs_init().
   Reads superblock: if valid, loads filesystem from disk.
   If blank disk, formats and syncs current VFS to disk.
   Returns 0 on success, -1 if no disk. */
int spikefs_init(void);

/* Write empty filesystem to disk. Layout calculated from disk size. */
int spikefs_format(void);

/* Serialize current in-memory VFS to disk. Clears dirty flag on success. */
int spikefs_sync(void);

/* Deserialize disk filesystem into in-memory VFS. */
int spikefs_load(void);

#endif
