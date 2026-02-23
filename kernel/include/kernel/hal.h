#ifndef _HAL_H
#define _HAL_H

#include <stdint.h>

/*
 * Hardware Abstraction Layer (HAL)
 *
 * Arch-independent interface for hardware operations. The kernel calls
 * these functions instead of using raw asm. Each architecture provides
 * its own implementation (e.g. kernel/arch/i386/hal.c).
 *
 * This is the boundary that enables porting: to add ARM support,
 * implement these functions in kernel/arch/arm/hal.c without changing
 * any kernel code above this layer.
 */

/* ------------------------------------------------------------------ */
/*  Interrupts                                                        */
/* ------------------------------------------------------------------ */

/* Disable interrupts, return previous interrupt state (for nesting) */
uint32_t hal_irq_save(void);

/* Restore interrupt state from hal_irq_save() */
void hal_irq_restore(uint32_t state);

/* Enable interrupts unconditionally */
void hal_irq_enable(void);

/* Disable interrupts unconditionally */
void hal_irq_disable(void);

/* ------------------------------------------------------------------ */
/*  CPU control                                                       */
/* ------------------------------------------------------------------ */

/* Halt until next interrupt */
void hal_halt(void);

/* Halt forever (disable interrupts + halt, for panic) */
void hal_halt_forever(void) __attribute__((noreturn));

/* ------------------------------------------------------------------ */
/*  I/O ports (x86) / MMIO (ARM)                                     */
/* ------------------------------------------------------------------ */

/* Read a byte from an I/O port (x86) or MMIO address (ARM) */
uint8_t hal_inb(uint16_t port);

/* Write a byte to an I/O port (x86) or MMIO address (ARM) */
void hal_outb(uint16_t port, uint8_t val);

/* Read a 16-bit word from an I/O port */
uint16_t hal_inw(uint16_t port);

/* Write a 16-bit word to an I/O port */
void hal_outw(uint16_t port, uint16_t val);

/* Read a 32-bit dword from an I/O port */
uint32_t hal_inl(uint16_t port);

/* Write a 32-bit dword to an I/O port */
void hal_outl(uint16_t port, uint32_t val);

/* Bulk 16-bit read from a port (x86 insw / ARM memcpy from MMIO) */
void hal_insw(uint16_t port, void *buf, uint32_t count);

/* Bulk 16-bit write to a port */
void hal_outsw(uint16_t port, const void *buf, uint32_t count);

/* ------------------------------------------------------------------ */
/*  TLB                                                               */
/* ------------------------------------------------------------------ */

/* Invalidate TLB entry for a virtual address */
void hal_tlb_invalidate(uint32_t vaddr);

/* Flush entire TLB (e.g. on CR3 switch) */
void hal_tlb_flush_all(void);

/* ------------------------------------------------------------------ */
/*  MMU / Page directory                                              */
/* ------------------------------------------------------------------ */

/* Get current page directory physical address */
uint32_t hal_get_cr3(void);

/* Set page directory physical address (switches address space) */
void hal_set_cr3(uint32_t pd_phys);

/* Get the faulting address (CR2 on x86, FAR on ARM) */
uint32_t hal_get_fault_addr(void);

#endif
