# kernel/drivers/

Hardware device drivers for SpikeOS.

## What's Here

- **timer.c** — PIT timer at 100Hz on IRQ0 (drives scheduler preemption)
- **keyboard.c** — PS/2 keyboard on IRQ1 with extended scan code support and blocking reads
- **uart.c** — COM1 serial port at 38400 baud on IRQ4 (output captured to `.debug.log`)
- **ata.c** — ATA PIO disk driver for primary IDE master (polling, 28-bit LBA)
- **pic.c** — 8259A PIC: remaps IRQs 0-15 to vectors 32-47, EOI handling
- **vga13.c** — VGA mode 13h graphics (320x200, 256-color) used by Tetris
- **framebuffer.c** — GOP/VBE linear framebuffer driver (save info from multiboot, map to kernel VA, pixel ops)
- **fb_console.c** — Framebuffer text console (8x16 CP437 glyph rendering, cursor, scroll, VGA color mapping)
- **debug_log.c** — NDJSON debug logger over UART

## How It Fits Together

All drivers register their IRQ handlers through `irq_install_handler()` in `kernel/core/isr.c` and use the HAL (`kernel/arch/i386/hal.c`) for port I/O. The timer drives the scheduler, the keyboard feeds the shell, the ATA driver backs SpikeFS, VGA13 provides graphics for games, and the framebuffer console provides high-resolution text output after the boot splash.
