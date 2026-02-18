# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

SpikeOS is a 32-bit x86 OS kernel built from scratch using GRUB/Multiboot, cross-compiled with `i386-elf-gcc`. It is a higher-half kernel (virtual base `0xC0000000`) loaded by GRUB at physical address `0x00200000`.

## Prerequisites (macOS/Apple Silicon)

```bash
brew install i386-elf-gcc nasm qemu grub xorriso mtools
```

## Build & Run Commands

```bash
./clean.sh        # Remove build artifacts
./headers.sh      # Install headers to sysroot (required before first build)
./qemu.sh         # Build everything and launch in QEMU
./iso.sh          # Build and create bootable ISO (myos.iso)
```

`./qemu.sh` is the primary development loop — it builds and runs the kernel, with UART serial output captured to `.debug.log`.

To inspect UART debug output during or after a run:
```bash
cat .debug.log
```

## Architecture

### Memory Layout

- Physical load address: `0x00200000` (GRUB loads here)
- Higher-half virtual base: `0xC0000000` (+3GiB offset)
- Identity-mapped first 4MB for bootstrap, then higher-half paging enabled
- Physical frame allocator: bitmap over `MAX_FRAMES = 16384` frames
- `endkernel` linker symbol marks end of kernel image (used to reserve physical frames)

### Kernel Initialization Order (`kernel/kernel/kernel.c`)

1. `terminal_initialize()` — VGA 80×25 text mode
2. `gdt_init()` — flat memory model GDT
3. `idt_init()` — 256-vector IDT
4. `paging_init()` + `paging_enable()` — enable CR0.PG, switch to higher-half
5. `pic_remap(0x20, 0x28)` — remap PIC IRQs away from CPU exceptions
6. `timer_init(100)` — 100Hz preemptive timer on IRQ0
7. `process_init()` / `scheduler_init()` — process table (max 32) + scheduler
8. `keyboard_init()` — keyboard on IRQ1
9. `uart_init()` + `irq_install_handler(4, uart_irq_handler)` — COM1 serial on IRQ4
10. `proc_create_kernel_thread(...)` — create kernel threads
11. `sti` — enable interrupts

### Directory Structure

| Path | Purpose |
|------|---------|
| `kernel/arch/i386/` | Boot stub (`boot.S`), GDT/IDT flush stubs, paging enable, linker script, VGA driver |
| `kernel/kernel/` | All C source: kernel entry, GDT, IDT, ISR/IRQ, paging, process, scheduler, PIC, timer, keyboard, UART, shell |
| `kernel/include/kernel/` | All kernel headers |
| `libc/` | Freestanding kernel libc (`libk.a`): printf, string, stdlib/abort |
| `sysroot/` | Staging install directory (headers + libraries) |
| `isodir/` | ISO staging directory with GRUB config |

### Interrupt Handling

- `kernel/arch/i386/isr_stub.S` — 31 exception stubs (vectors 0–30), push trapframe, jump to `isr_common_handler`
- `kernel/arch/i386/irq_stub.S` — 16 hardware IRQ stubs (vectors 32–47)
- `kernel/kernel/isr.c` — dispatcher that calls registered handlers
- Register handlers via `irq_install_handler(irq_num, handler_fn)`

### Paging

- Two-level page tables: `page_directory[1024]` → `page_table[1024]`
- `paging_init()` sets up identity map + higher-half map before enabling paging
- `map_page(phys, virt, flags)` — dynamic mapping with `invlpg` TLB invalidation
- `alloc_frame()` / `free_frame()` — bitmap physical frame allocator
- `virt_to_phys(vaddr)` — walk page tables to resolve physical address

### Process & Scheduling

- Process states: `NEW → READY → RUNNING → BLOCKED → ZOMBIE`
- Each process has its own kernel stack and saved CPU context (ESP/EBP)
- Scheduler is timer-driven (fires on each IRQ0 tick)
- `proc_create_kernel_thread(fn)` — create a kernel-mode thread

## Current Known Issues

- `proc_create_kernel_thread(thread_inc)` causes a page fault — stack/memory setup for new threads needs debugging (see `kernel.c:145`)
