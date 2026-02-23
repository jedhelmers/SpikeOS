# kernel/core/

CPU core management and kernel entry point.

## What's Here

- **kernel.c** — `kernel_main()`: full initialization sequence from GDT through shell startup
- **gdt.c** — Global Descriptor Table (6 entries: null, kernel code/data, user code/data, TSS)
- **idt.c** — Interrupt Descriptor Table (256 vectors: exceptions, IRQs, syscall at 0x80)
- **isr.c** — Central interrupt dispatcher: routes exceptions, syscalls, and IRQs
- **tss.c** — Task State Segment for ring-3 to ring-0 transitions
- **syscall.c** — 19 system calls via `int $0x80` (table-driven dispatch)

## How It Fits Together

`kernel_main()` initializes all subsystems in order: GDT, TSS, IDT, PIC, paging, heap, initrd, ATA, VFS, SpikeFS, timer, processes, keyboard, UART, then starts the shell as a kernel thread.

All interrupts flow through `isr_common_handler()` in `isr.c`, which dispatches to exception handlers, the syscall table, or registered IRQ handlers. The scheduler integrates via the timer IRQ returning a new stack pointer for context switching.
