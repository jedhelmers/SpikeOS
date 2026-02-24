#ifndef _MULTIBOOT_H
#define _MULTIBOOT_H

#include <stdint.h>

/* Multiboot info flags bits */
#define MB_FLAG_MEM         (1 << 0)    /* mem_lower/mem_upper valid */
#define MB_FLAG_BOOTDEV     (1 << 1)    /* boot_device valid */
#define MB_FLAG_CMDLINE     (1 << 2)    /* cmdline valid */
#define MB_FLAG_MODS        (1 << 3)    /* mods_count/mods_addr valid */
#define MB_FLAG_FRAMEBUFFER (1 << 12)   /* framebuffer info valid */

/*
 * Full Multiboot 1 info structure.
 * Fields are at fixed offsets defined by the spec.
 * We include all fields up through framebuffer info.
 */
struct multiboot_info {
    /* offset 0x00 */
    uint32_t flags;
    uint32_t mem_lower;
    uint32_t mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    /* offset 0x1C — syms (aout/ELF, unused) */
    uint32_t syms[4];
    /* offset 0x2C — memory map */
    uint32_t mmap_length;
    uint32_t mmap_addr;
    /* offset 0x34 — drives */
    uint32_t drives_length;
    uint32_t drives_addr;
    /* offset 0x3C — config + boot loader */
    uint32_t config_table;
    uint32_t boot_loader_name;
    /* offset 0x44 — APM table */
    uint32_t apm_table;
    /* offset 0x48 — VBE info */
    uint32_t vbe_control_info;
    uint32_t vbe_mode_info;
    uint16_t vbe_mode;
    uint16_t vbe_interface_seg;
    uint16_t vbe_interface_off;
    uint16_t vbe_interface_len;
    /* offset 0x58 — framebuffer info (valid when flags bit 12 set) */
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t  framebuffer_bpp;
    uint8_t  framebuffer_type;      /* 0=indexed, 1=RGB, 2=EGA text */
    uint8_t  fb_red_pos, fb_red_mask;
    uint8_t  fb_green_pos, fb_green_mask;
    uint8_t  fb_blue_pos, fb_blue_mask;
} __attribute__((packed));

/* Module entry (16 bytes each, at mods_addr) */
struct multiboot_mod_entry {
    uint32_t mod_start;     /* physical start address */
    uint32_t mod_end;       /* physical end address (exclusive) */
    uint32_t string;        /* physical address of command-line string */
    uint32_t reserved;
};

#endif
