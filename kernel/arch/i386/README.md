# kernel/arch/i386/

x86 32-bit architecture support for SpikeOS.

## What's Here

- **boot.S** — Multiboot entry, bootstrap paging, stack setup, jump to higher-half kernel
- **isr_stub.S / irq_stub.S** — Assembly stubs for CPU exceptions (0-31), syscall (128), and hardware IRQs (32-47)
- **gdt_flush.S / idt_load.S / tss_flush.S** — CPU descriptor table loading stubs
- **paging_enable.S** — Enables paging by setting CR3 and CR0.PG
- **tty.c** — VGA 80x25 text-mode terminal with 200-line scrollback and UEFI mode-3 support
- **hal.c** — Hardware Abstraction Layer (port I/O, IRQ save/restore, TLB, CR3, halt)
- **linker.ld** — Linker script: physical load at 0x200000, virtual at 0xC0000000+

## How It Fits Together

The boot process starts in `boot.S` (Multiboot entry), sets up temporary page tables for higher-half mapping, then jumps to `kernel_main` in `kernel/core/kernel.c`. The HAL (`hal.c`) provides a portable interface for all hardware operations used throughout the kernel. The terminal driver (`tty.c`) manages VGA text output with scrollback history.

All interrupt/exception stubs push a uniform trapframe struct onto the stack before calling the C dispatcher in `kernel/core/isr.c`.
