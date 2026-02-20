#ifndef _MULTIBOOT_H
#define _MULTIBOOT_H

#include <stdint.h>

/* Multiboot info flags bits */
#define MB_FLAG_MEM      (1 << 0)   /* mem_lower/mem_upper valid */
#define MB_FLAG_BOOTDEV  (1 << 1)   /* boot_device valid */
#define MB_FLAG_CMDLINE  (1 << 2)   /* cmdline valid */
#define MB_FLAG_MODS     (1 << 3)   /* mods_count/mods_addr valid */

/* Multiboot info structure (subset — only fields we use) */
struct multiboot_info {
    uint32_t flags;
    uint32_t mem_lower;
    uint32_t mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
};

/* Module entry (16 bytes each, at mods_addr) */
struct multiboot_mod_entry {
    uint32_t mod_start;     /* physical start address */
    uint32_t mod_end;       /* physical end address (exclusive) */
    uint32_t string;        /* physical address of command-line string */
    uint32_t reserved;
};

#endif
