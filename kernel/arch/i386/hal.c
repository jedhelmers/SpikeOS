#include <kernel/hal.h>

/*
 * HAL implementation for i386 (x86 32-bit).
 *
 * These are thin wrappers around x86 instructions. An ARM port would
 * replace this file with ARM equivalents (cpsid/cpsie, ldr/str to
 * MMIO, MCR/MRC for coprocessor regs, etc.).
 */

/* ------------------------------------------------------------------ */
/*  Interrupts                                                        */
/* ------------------------------------------------------------------ */

uint32_t hal_irq_save(void) {
    uint32_t flags;
    asm volatile("pushfl; popl %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

void hal_irq_restore(uint32_t state) {
    asm volatile("pushl %0; popfl" :: "r"(state) : "memory");
}

void hal_irq_enable(void) {
    asm volatile("sti" ::: "memory");
}

void hal_irq_disable(void) {
    asm volatile("cli" ::: "memory");
}

/* ------------------------------------------------------------------ */
/*  CPU control                                                       */
/* ------------------------------------------------------------------ */

void hal_halt(void) {
    asm volatile("hlt");
}

void hal_halt_forever(void) {
    asm volatile("cli");
    for (;;) asm volatile("hlt");
    __builtin_unreachable();
}

/* ------------------------------------------------------------------ */
/*  I/O ports                                                         */
/* ------------------------------------------------------------------ */

uint8_t hal_inb(uint16_t port) {
    uint8_t ret;
    asm volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

void hal_outb(uint16_t port, uint8_t val) {
    asm volatile("outb %0, %1" :: "a"(val), "Nd"(port));
}

uint16_t hal_inw(uint16_t port) {
    uint16_t ret;
    asm volatile("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

void hal_outw(uint16_t port, uint16_t val) {
    asm volatile("outw %0, %1" :: "a"(val), "Nd"(port));
}

uint32_t hal_inl(uint16_t port) {
    uint32_t ret;
    asm volatile("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

void hal_outl(uint16_t port, uint32_t val) {
    asm volatile("outl %0, %1" :: "a"(val), "Nd"(port));
}

void hal_insw(uint16_t port, void *buf, uint32_t count) {
    asm volatile("rep insw"
                 : "+D"(buf), "+c"(count)
                 : "d"(port)
                 : "memory");
}

void hal_outsw(uint16_t port, const void *buf, uint32_t count) {
    asm volatile("rep outsw"
                 : "+S"(buf), "+c"(count)
                 : "d"(port)
                 : "memory");
}

/* ------------------------------------------------------------------ */
/*  TLB                                                               */
/* ------------------------------------------------------------------ */

void hal_tlb_invalidate(uint32_t vaddr) {
    asm volatile("invlpg (%0)" :: "r"(vaddr) : "memory");
}

void hal_tlb_flush_all(void) {
    uint32_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    asm volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
}

/* ------------------------------------------------------------------ */
/*  MMU / Page directory                                              */
/* ------------------------------------------------------------------ */

uint32_t hal_get_cr3(void) {
    uint32_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

void hal_set_cr3(uint32_t pd_phys) {
    asm volatile("mov %0, %%cr3" :: "r"(pd_phys) : "memory");
}

uint32_t hal_get_fault_addr(void) {
    uint32_t cr2;
    asm volatile("mov %%cr2, %0" : "=r"(cr2));
    return cr2;
}
