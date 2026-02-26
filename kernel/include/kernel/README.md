# kernel/include/kernel/

All kernel header files, organized in a flat namespace.

## Overview

Headers are included as `#include <kernel/header.h>`. They define the public API for each kernel subsystem: data structures, function declarations, constants, and static initializers.

## Headers by Category

**Architecture:** `hal.h`, `io.h`, `multiboot.h`, `tty.h`, `key_event.h`

**Core CPU:** `gdt.h`, `idt.h`, `tss.h`, `isr.h`, `syscall.h`, `signal.h`

**Memory:** `paging.h`, `heap.h`

**Filesystem:** `vfs.h`, `spikefs.h`, `initrd.h`, `fd.h`, `pipe.h`

**Process:** `process.h`, `scheduler.h`, `elf.h`, `wait.h`, `mutex.h`, `condvar.h`, `rwlock.h`

**Drivers:** `keyboard.h`, `timer.h`, `uart.h`, `ata.h`, `pic.h`, `vga13.h`, `framebuffer.h`, `fb_console.h`, `mouse.h`, `event.h`, `window.h`, `dock.h`, `surface.h`, `pci.h`, `e1000.h`, `debug_log.h`

**Networking:** `net.h`

**Shell:** `shell.h`, `boot_splash.h`, `tetris.h`, `editor.h`, `gui_editor.h`

**System:** `settings.h`

## Conventions

- Include guard format: `#ifndef _FOO_H` / `#define _FOO_H`
- Static initializers for stack/global objects: `MUTEX_INIT`, `CONDVAR_INIT`, `RWLOCK_INIT`, `WAIT_QUEUE_INIT`
- Hardware abstraction through `hal.h` (prefer over direct `io.h` port access)
