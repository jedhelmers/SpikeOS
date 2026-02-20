#ifndef _TSS_H
#define _TSS_H

#include <stdint.h>

/*
 * x86 32-bit Task State Segment (104 bytes).
 *
 * The CPU reads esp0/ss0 on every ring-3 -> ring-0 transition
 * (interrupt, exception, or syscall) to find the kernel stack.
 * All other fields are unused in the single-TSS software-switching
 * model — they exist only because the hardware defines them.
 */
struct tss_entry {
    uint32_t prev_tss;   /* 0x00 - link to previous TSS (unused) */
    uint32_t esp0;       /* 0x04 - kernel stack pointer for ring-0 */
    uint32_t ss0;        /* 0x08 - kernel stack segment (0x10) */
    uint32_t esp1;       /* 0x0C */
    uint32_t ss1;        /* 0x10 */
    uint32_t esp2;       /* 0x14 */
    uint32_t ss2;        /* 0x18 */
    uint32_t cr3;        /* 0x1C */
    uint32_t eip;        /* 0x20 */
    uint32_t eflags;     /* 0x24 */
    uint32_t eax;        /* 0x28 */
    uint32_t ecx;        /* 0x2C */
    uint32_t edx;        /* 0x30 */
    uint32_t ebx;        /* 0x34 */
    uint32_t esp;        /* 0x38 */
    uint32_t ebp;        /* 0x3C */
    uint32_t esi;        /* 0x40 */
    uint32_t edi;        /* 0x44 */
    uint32_t es;         /* 0x48 */
    uint32_t cs;         /* 0x4C */
    uint32_t ss;         /* 0x50 */
    uint32_t ds;         /* 0x54 */
    uint32_t fs;         /* 0x58 */
    uint32_t gs;         /* 0x5C */
    uint32_t ldt;        /* 0x60 */
    uint16_t trap;       /* 0x64 - debug trap flag */
    uint16_t iomap_base; /* 0x66 - I/O bitmap offset; sizeof(tss) = no bitmap */
} __attribute__((packed));

extern struct tss_entry kernel_tss;

/*
 * Initialize the TSS: zero all fields, set ss0/esp0/iomap_base,
 * install the TSS descriptor into GDT slot 5, and load TR.
 * Must be called after gdt_init().
 */
void tss_init(void);

/*
 * Update the kernel stack pointer in the TSS.
 * Called by the scheduler on every context switch so the CPU
 * uses the correct kernel stack when the next process traps in.
 */
void tss_set_kernel_stack(uint32_t esp0);

/* Assembly: loads the Task Register with the given selector. */
extern void tss_flush(uint32_t selector);

#endif
