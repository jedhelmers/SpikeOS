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

`kernel_main()` initializes all subsystems in order: GDT, TSS, IDT, PIC, paging, heap, framebuffer, initrd, ATA, VFS, SpikeFS, fd/pipe/event, process/scheduler, then timer + device IRQs, boot splash, window manager, and finally the shell as a kernel thread.

**Critical ordering**: `process_init()` and `scheduler_init()` must complete before `timer_init()` + `pic_clear_mask(0)`, because the timer IRQ triggers `scheduler_tick()` which requires `current_process` and `kernel_cr3` to be initialized. All syscalls validate user pointers (reject addresses >= `KERNEL_VMA_OFFSET`).

All interrupts flow through `isr_common_handler()` in `isr.c`, which dispatches to exception handlers, the syscall table, or registered IRQ handlers. The scheduler integrates via the timer IRQ returning a new stack pointer for context switching.
