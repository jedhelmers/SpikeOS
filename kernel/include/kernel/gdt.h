#ifndef _GDT_H
#define _GDT_H
#include <stdint.h>

/*
    We are creating the OS's memoty and privilege model's birth cert.

 */

struct gdt_entry {
    uint16_t limit_low;     // Lower 16 bits of the segment limit
    uint16_t base_low;      // Lower 16 bits of the base address. In flat mode this is 0
    uint8_t base_mid;       // Middle 8 of the base address
    uint8_t access;         // Access byte. Controls: Present bit, Ring level, Code v. Data, R/W permissions
    uint8_t gran;           // Upper 4 bits of the limiit/flags (granularity)
    uint8_t base_high;      // Top 8 bits of the base address.
} __attribute__((packed));  // DO NOT INSERT PADDING BETWEEN FIELDS

/*
    This describes the format that lgdt expects in memory
 */
struct gdt_ptr {
    uint16_t limit;         // Size of GDT in bytes - 1
    uint32_t base;          // Address of the GDT array
} __attribute__((packed));  // DO NOT INSERT PADDING BETWEEN FIELDS


// This tells C that there's an assembly function somewhere that takes a pointer and runs "lgdt"
extern void gdt_flush(uint32_t);

void gdt_init(void);

/*
 * Install the TSS descriptor into GDT slot 5 (selector 0x28).
 * Called by tss_init() after the TSS struct is initialized.
 */
void gdt_install_tss(uint32_t base, uint32_t limit, uint8_t access, uint8_t gran);

#endif