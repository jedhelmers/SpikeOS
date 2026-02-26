# kernel/drivers/

Hardware device drivers for SpikeOS.

## What's Here

- **timer.c** — PIT timer at 100Hz on IRQ0 (drives scheduler preemption)
- **keyboard.c** — PS/2 keyboard on IRQ1 with extended scan code support (arrows, Page Up/Down, Home, End, Insert, Delete) and blocking reads
- **uart.c** — COM1 serial port at 38400 baud on IRQ4 (output captured to `.debug.log`)
- **ata.c** — ATA PIO disk driver for primary IDE master (polling, 28-bit LBA), interrupt-safe via `hal_irq_save/restore`
- **pic.c** — 8259A PIC: remaps IRQs 0-15 to vectors 32-47, EOI handling
- **vga13.c** — VGA mode 13h graphics (320x200, 256-color) used by Tetris
- **framebuffer.c** — GOP/VBE linear framebuffer driver (save info from multiboot, map to kernel VA, pixel ops, XRGB8888 color packing)
- **fb_console.c** — Framebuffer text console (8x16 CP437 glyph rendering, visible cursor, 200-line scrollback, VGA color mapping)
- **event.c** — Unified event queue (keyboard + mouse events) with blocking wait support, interrupt-safe via `hal_irq_save/restore`
- **mouse.c** — PS/2 mouse driver on IRQ12 with software cursor (save/restore background)
- **window.c** — Window manager: doubly-linked window list with z-order, click-to-focus, corner-only resize, desktop/per-window menu bars, dropdown menus, right-click context menus, desktop icons, traffic light dots, AA rounded corners, cooperative close via `WIN_FLAG_CLOSE_REQ`
- **dock.c** — macOS-style dock: rounded pill, procedural app icons (Shell/Editor/Tetris/Finder), hover tooltips, running indicators, app launching
- **surface.c** — Offscreen XRGB8888 pixel buffer: create/destroy, fill rect, glyph rendering (1-3x scaling), blit to framebuffer or other surfaces
- **pci.c** — PCI bus 0 enumeration, config space read/write (ports 0x0CF8/0x0CFC), device search, bus mastering enable
- **e1000.c** — Intel e1000 NIC driver: MMIO at PDE[771], TX (16 desc) / RX (32 desc) descriptor rings, EEPROM MAC read, IRQ-driven receive
- **debug_log.c** — NDJSON debug logger over UART

## How It Fits Together

All drivers register their IRQ handlers through `irq_install_handler()` in `kernel/core/isr.c` and use the HAL (`kernel/arch/i386/hal.c`) for port I/O and interrupt management. The timer drives the scheduler, the keyboard and mouse feed the event queue, the ATA driver backs SpikeFS, the framebuffer console provides high-resolution text output, and the window manager draws desktop chrome and manages window positioning. The dock provides app launching and running indicators. The surface system enables offscreen rendering for the GUI editor and Finder. The PCI driver discovers devices on bus 0, and the e1000 NIC driver provides Ethernet connectivity for the networking stack.
