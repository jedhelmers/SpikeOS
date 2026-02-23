# SpikeOS

An x86 learning experiment in loving memory of my boy, Spike.

## Overview

SpikeOS is a 32-bit x86 OS kernel built from scratch using GRUB/Multiboot, cross-compiled with `i386-elf-gcc`. It is a higher-half kernel (virtual base `0xC0000000`) loaded by GRUB at physical address `0x00200000`.

Working through the wonderful resources at [wiki.osdev.org](https://wiki.osdev.org), PDFs from the [University of Wisconsin-Madison](https://pages.cs.wisc.edu/~remzi/OSTEP/#book-chapters), and the classic book *OPERATING SYSTEMS - Design & Implementation* by Andrew S. Tanenbaum.

## Prerequisites (macOS/Apple Silicon)

```bash
brew install i386-elf-gcc i686-elf-grub x86_64-elf-grub nasm qemu xorriso mtools
```

| Tool | Purpose |
|------|---------|
| `i386-elf-gcc` | Cross-compiler for building the kernel |
| `nasm` | Assembler (x86 assembly) |
| `qemu` | Emulator to run the OS |
| `i686-elf-grub` / `x86_64-elf-grub` | Bootloader (BIOS + UEFI) |
| `xorriso` | ISO creation tool |
| `mtools` | FAT filesystem tools (used in boot/ISO workflows) |

For UEFI boot support (hybrid BIOS+UEFI ISO), run the one-time setup:
```bash
make setup-efi
```

## Build & Run

```bash
make run                # Build everything and launch in QEMU (BIOS)
make run-verbose        # Build and launch with verbose boot logging (no splash screen)
make run-uefi           # Build everything and launch in QEMU (UEFI via OVMF)
make run-uefi-verbose   # Build and launch UEFI with verbose boot logging
make iso                # Build and create bootable ISO (myos.iso)
make clean              # Remove build artifacts
make headers            # Install headers to sysroot (required before first build)
make setup-efi          # One-time UEFI support setup
```

Scripts can also be called directly: `scripts/qemu.sh`, `scripts/clean.sh`, etc.

`make run` is the primary development loop — it builds and runs the kernel, with UART serial output captured to `.debug.log`. By default, the kernel shows a 1980s-style boot splash; pass `-v` or `--verbose` to see init logging instead.

```bash
cat .debug.log    # Inspect UART debug output during or after a run
```

## Architecture

### Memory Layout

- Physical load address: `0x00200000` (GRUB loads here)
- Higher-half virtual base: `0xC0000000` (+3 GiB offset)
- Identity-mapped first 4 MB for bootstrap, then higher-half paging enabled
- Physical frame allocator: bitmap over `MAX_FRAMES = 16384` frames (64 MB addressable)
- `endkernel` linker symbol marks end of kernel image (used to reserve physical frames)
- Kernel heap at `0xC0400000` (PDE[769]), up to 4 MB, first-fit free-list allocator

### GDT Layout

| Index | Selector | Access | Description |
|-------|----------|--------|-------------|
| 0 | 0x00 | 0x00 | Null |
| 1 | 0x08 | 0x9A | Kernel code (DPL=0) |
| 2 | 0x10 | 0x92 | Kernel data (DPL=0) |
| 3 | 0x18 | 0xFA | User code (DPL=3) |
| 4 | 0x20 | 0xF2 | User data (DPL=3) |
| 5 | 0x28 | 0x89 | TSS descriptor |

### Boot Sequence

All init `printf` calls are wrapped in `#ifdef VERBOSE_BOOT`. Without the flag, init is silent and a retro boot splash is shown instead.

1. `terminal_initialize()` — VGA 80x25 text mode (includes `vga_set_mode3()` for UEFI compatibility)
2. `gdt_init()` — 6-entry GDT (null, kernel code/data, user code/data, TSS placeholder)
3. `tss_init()` — TSS with `ss0=0x10`, `esp0` set to current kernel stack
4. `idt_init()` — 256-vector IDT, including syscall gate at vector 0x80 (DPL=3)
5. `pic_remap(0x20, 0x28)` — remap PIC IRQs away from CPU exceptions
6. `paging_init()` + `paging_enable()` — enable CR0.PG, switch to higher-half
7. `heap_init()` — kernel heap allocator (kmalloc/kfree)
8. `initrd_init()` — parse GRUB module into file entry table
9. `ata_init()` — ATA PIO disk driver (primary master, 28-bit LBA)
10. `vfs_init(64)` — in-memory inode filesystem (starts with 64 slots, grows on demand up to 8192)
11. `vfs_import_initrd()` — copy initrd files into VFS root
12. `spikefs_init()` — load filesystem from disk (or format if blank/incompatible)
13. `timer_init(100)` — 100 Hz preemptive timer on IRQ0
14. `fd_init()` / `pipe_init()` — zero the system-wide open file table and pipe pool
15. `process_init()` / `scheduler_init()` — process table (max 32) + round-robin scheduler
16. `keyboard_init()` — keyboard on IRQ1
17. `uart_init()` — COM1 serial on IRQ4
18. `boot_splash()` — 1980s-style animated boot screen (only without `VERBOSE_BOOT`), waits for key press
19. `proc_create_kernel_thread(shell_run)` — start the kernel shell
20. `sti` — enable interrupts

### Directory Structure

| Path | Purpose |
|------|---------|
| `kernel/arch/i386/` | Boot stub (`boot.S`), GDT/IDT/TSS flush stubs, paging enable, HAL, linker script, VGA driver |
| `kernel/core/` | Kernel entry, GDT, IDT, ISR, TSS, syscall dispatch |
| `kernel/mm/` | Paging (page directory/tables, frame allocator, page fault handler) and heap allocator |
| `kernel/fs/` | VFS, SpikeFS on-disk filesystem, initrd, file descriptors, pipes |
| `kernel/drivers/` | ATA disk, keyboard, UART, PIC, timer, VGA mode 13h, debug log |
| `kernel/proc/` | Process table, scheduler, ELF loader, wait queues |
| `kernel/shell/` | Kernel shell, Tetris, boot splash |
| `kernel/include/kernel/` | All kernel headers (flat) |
| `libc/` | Freestanding kernel libc (`libk.a`): printf, string, stdlib/abort |
| `scripts/` | Build, run, and setup scripts |
| `tools/` | Host-side build tools (mkinitrd) |
| `userland/` | User-mode test programs |
| `sysroot/` | Staging install directory (headers + libraries) |
| `isodir/` | ISO staging directory with GRUB config |

## Subsystems

### Interrupt Handling

- `isr_stub.S` — 32 exception stubs (vectors 0-31) + syscall stub (vector 128)
- `irq_stub.S` — 16 hardware IRQ stubs (vectors 32-47)
- `isr.c` — dispatcher: routes syscalls to `syscall_dispatch()`, page faults (ISR 14) to `page_fault_handler()`, other exceptions to halt, IRQs to registered handlers
- Register handlers via `irq_install_handler(irq_num, handler_fn)`

### Paging

- Two-level page tables: `page_directory[1024]` -> `page_table[1024]`
- `paging_init()` sets up identity map (PDE[0]) + higher-half map (PDE[768]) + heap table (PDE[769])
- `map_page(virt, phys, flags)` — dynamic mapping with `invlpg` TLB invalidation
- `alloc_frame()` / `free_frame()` — bitmap physical frame allocator
- `virt_to_phys(vaddr)` — walk page tables to resolve physical address
- `temp_map(phys)` / `temp_unmap()` — temporary mapping window at `0xC03FF000` for accessing physical frames that aren't identity-mapped

### Per-Process Page Directories

- Each process has a `cr3` field (physical address of its page directory)
- `pgdir_create()` — allocates a physical frame, clones all 1024 kernel PDEs via `temp_map`
- `pgdir_map_user_page()` — maps a page in a specific process's PD; auto-clones shared kernel page tables when `PAGE_USER` is needed
- `pgdir_destroy()` — frees user page tables and the PD frame
- Scheduler switches CR3 on context switch when processes have different page directories

### Heap Allocator

- First-fit free-list with block splitting and forward/backward coalescing
- `kmalloc(size)` / `kfree(ptr)` / `kcalloc(n, size)` / `krealloc(ptr, size)`
- 16-byte aligned, interrupt-safe (cli/sti around critical sections)
- `heap_dump()` — debug output, used by `meminfo` shell command

### TSS (Task State Segment)

- Single global TSS (104-byte x86 struct)
- `esp0` updated on every context switch via `tss_set_kernel_stack()`
- `ss0 = 0x10` (kernel data segment)
- Enables ring-3 -> ring-0 transitions on interrupts

### Syscall Interface

- `int $0x80` with IDT gate DPL=3
- Convention: EAX=syscall number, EBX/ECX/EDX=args, return value in EAX

| # | Name | Args | Description |
|---|------|------|-------------|
| 0 | `SYS_EXIT` | EBX=status | Close fds, free PD, wake parent, mark ZOMBIE |
| 1 | `SYS_WRITE` | EBX=fd, ECX=buf, EDX=len | Write to fd (console, VFS file, or pipe) |
| 2 | `SYS_READ` | EBX=fd, ECX=buf, EDX=len | Read from fd (blocks if pipe empty) |
| 3 | `SYS_OPEN` | EBX=path, ECX=flags | Open VFS file, returns fd |
| 4 | `SYS_CLOSE` | EBX=fd | Close file descriptor |
| 5 | `SYS_SEEK` | EBX=fd, ECX=off, EDX=whence | Seek within VFS file |
| 6 | `SYS_STAT` | EBX=path, ECX=&stat | Get file info (type, size, inode, nlink) |
| 7 | `SYS_GETPID` | — | Return current PID |
| 8 | `SYS_SLEEP` | EBX=ticks | Sleep for N ticks (10ms each) |
| 9 | `SYS_BRK` | EBX=addr | Adjust process break (stub) |
| 10 | `SYS_SPAWN` | EBX=name | Spawn ELF from initrd |
| 11 | `SYS_WAITPID` | EBX=pid, ECX=&status | Wait for child to exit |
| 12 | `SYS_MKDIR` | EBX=path | Create directory |
| 13 | `SYS_UNLINK` | EBX=path | Remove file or empty directory |
| 14 | `SYS_CHDIR` | EBX=path | Change working directory |
| 15 | `SYS_GETCWD` | EBX=buf, ECX=size | Get current working directory |
| 16 | `SYS_PIPE` | EBX=int[2] | Create pipe (read/write fd pair) |
| 17 | `SYS_DUP` | EBX=fd | Duplicate file descriptor |

### Process & Scheduling

- Process states: `NEW -> READY -> RUNNING -> BLOCKED -> ZOMBIE`
- Each process has its own kernel stack (4 KB), saved CPU context (ESP/EBP), page directory (CR3), and file descriptor table (16 fds)
- Process hierarchy: parent_pid, exit_status, wait queue for waitpid blocking
- Round-robin scheduler, timer-driven (fires on each IRQ0 tick at 100 Hz)
- `proc_create_kernel_thread(fn)` — create a kernel-mode thread, initializes stdin/stdout/stderr
- `proc_create_user_process(pd_phys, eip, esp)` — create a ring-3 process with its own page directory
- `elf_spawn(name)` — spawn user process from initrd ELF, sets parent_pid
- Context switch: saves trapframe + ESP, picks next READY process, updates TSS esp0, switches CR3 via HAL

### Virtual File System (VFS)

In-memory inode-based filesystem (like Linux's page cache). All data lives in kmalloc'd heap buffers; SpikeFS handles persistence to disk.

- **Inode table**: heap-allocated, starts at 64 slots, grows on demand via `krealloc` (doubles when full, up to 8192 — btrfs/XFS-style dynamic allocation)
- **Inode types**: free (0), file (1), directory (2)
- **Files**: `data` points to a kmalloc'd byte buffer, `size` = byte count
- **Directories**: `data` points to a dynamic array of dirents (name + inode number), grows via krealloc
- **Directory entries**: 64 bytes each (60-byte name + 4-byte inode number)
- **Root directory**: always inode 0, contains `.` and `..` entries pointing to itself
- **Path resolution**: iterative walk, starts from root (absolute) or cwd (relative), handles `.` and `..`
- **Dirty tracking**: global dirty flag set on any mutation; supports write-back to disk
- **Link counting**: inodes freed when link count reaches 0

### ATA Disk Driver

Polling-based PIO driver for the primary IDE master drive (port 0x1F0).

- `ata_init()` — IDENTIFY command, detects drive presence and sector count
- `ata_read_sectors()` / `ata_write_sectors()` — 28-bit LBA, 1-255 sectors
- `ata_flush()` — flush write cache
- QEMU disk: 64 MiB raw image (`disk.img`, 131072 sectors), attached as IDE primary master

### SpikeFS (On-Disk Filesystem — v3)

Persistent filesystem inspired by btrfs/XFS: inode chunks in a unified data pool. No fixed inode table — inodes are allocated from the same block pool as file data.

**On-disk layout:**
```
Sector 0:            Superblock (magic=0x534B4653 "SKFS", version=3)
Sectors 1..B:        Block bitmap (1 bit per data block)
Sectors B+1..end:    Data pool (inode chunks + file data, unified)
```

- **Inode chunks**: 8 inodes (64 bytes each) per 512-byte block, allocated from the data pool
- **Inode map**: chain of blocks (127 chunk entries + 1 "next" pointer per block), tracks which blocks are inode chunks
- **On-disk inode** (64 bytes): type, link_count, size, 12 direct block pointers, 1 indirect block pointer
- **On-disk dirent** (64 bytes): 60-byte name + 4-byte inode number
- **Full write-back**: sync clears bitmap and reallocates everything fresh from the in-memory VFS
- **Auto write-back**: shell prompt checks dirty flag + 5-second cooldown, auto-syncs if needed
- **Version detection**: auto-reformats disks with incompatible versions on boot

### Hardware Abstraction Layer (HAL)

Arch-independent interface (`kernel/include/kernel/hal.h`) with i386 implementation (`kernel/arch/i386/hal.c`). Wraps x86 instructions (CLI/STI, IN/OUT, INVLPG, CR3/CR2 access) behind portable function calls. An ARM/RISC-V port would replace `hal.c` with equivalent implementations.

### File Descriptors

Per-process fd table (16 fds) backed by a system-wide open file pool (64 entries). Supports VFS files, console (terminal), and pipe endpoints. Each process starts with stdin (0), stdout (1), stderr (2) pointing to the console. Operations: open, close, read, write, seek, dup.

### Pipes

Kernel IPC via 512-byte circular buffer. Blocking: read sleeps when empty (via wait queue), write sleeps when full. EOF on read when no writers remain; broken pipe on write when no readers remain. Created via `pipe_create()` which returns a read/write fd pair.

### Wait Queues

Blocking mechanism for processes. `sleep_on(wq)` sets process to BLOCKED and spins until woken. `wake_up_one(wq)` / `wake_up_all(wq)` set blocked processes back to READY. Used by pipes (read/write blocking) and waitpid (parent waits for child exit).

### Page Fault Handler

ISR 14 handler reads CR2 (faulting address). User-mode faults kill the process and yield to the scheduler. Kernel-mode faults print a register dump and halt (unrecoverable).

### Terminal & Scrollback

VGA 80x25 text mode driver with color output, cursor management, and a 200-line scrollback buffer.

- **Scrollback**: lines that scroll off the top of the screen are saved in a ring buffer (~32 KB in BSS)
- **Page Up / Page Down**: scroll through terminal history; any new output snaps back to the live view
- **Clear resets scrollback**: the `clear` command zeros the ring buffer

### Boot Splash

1980s-style retro boot screen shown by default (suppressed with `-v` flag). Green-on-black color scheme using CP437 box drawing and block characters.

- "SPIKE OS" logo in large block characters (5 rows tall)
- 4 animated system check stages with progress bar and `[ OK ]` results
- Animated progress bar that fills character-by-character
- "Press any key to continue..." prompt — temporarily enables interrupts for keyboard input
- Busy-wait timing for animations (interrupts enabled only for the key-press wait)

### UEFI Boot Support

The ISO is a hybrid BIOS+UEFI image. GRUB handles both boot paths — the kernel itself is unchanged.

- **BIOS path**: GRUB uses i386-pc modules, loads kernel via Multiboot, VGA already in text mode
- **UEFI path**: GRUB uses x86_64-efi modules, transitions CPU from long mode to 32-bit protected mode before loading the Multiboot kernel
- `vga_set_mode3()` handles the UEFI display: disables Bochs VBE, reprograms VGA registers for mode 3 (80x25 text), and loads the embedded 8x16 font into VGA plane 2
- Known limitation: `vga_set_mode3()` relies on Bochs VBE I/O ports, which only exist on QEMU/Bochs. Real UEFI hardware would need a framebuffer console.

## Shell Commands

Shell prompt shows current working directory: `jedhelmers:/path> `

| Command | Description |
|---------|-------------|
| `help` | Show available commands |
| `pwd` | Print working directory |
| `ls [path]` | List directory contents (defaults to cwd) |
| `cd <path>` | Change directory (supports `.`, `..`, absolute paths) |
| `mkdir <name>` | Create directory |
| `touch <name>` | Create empty file |
| `rm <name>` | Remove file or empty directory |
| `cat <name>` | Display file contents |
| `write <name> <text>` | Write text to file (auto-creates if missing) |
| `mv <src> <dst>` | Move/rename file or directory |
| `cp <src> <dst>` | Copy file |
| `sync` | Immediately persist filesystem to disk |
| `format` | Reformat disk (erases all data!) |
| `exec <name>` | Run ELF binary from initrd |
| `run` | Start test thread |
| `run tetris` | Play Tetris (WASD=move, Space=drop, Q=quit) |
| `ps` | List processes |
| `kill <pid>` | Kill process by PID |
| `meminfo` | Show heap info |
| `test <name>` | Run kernel tests: `fd`, `pipe`, `sleep`, `stat`, `waitpid`, or `all` |
| `clear` | Clear screen |

## Key Files

| File | Purpose |
|------|---------|
| `kernel/core/kernel.c` | Kernel entry point, init sequence, test functions |
| `kernel/core/gdt.c` | GDT with kernel + user segments + TSS |
| `kernel/core/isr.c` | Interrupt/exception/syscall dispatcher |
| `kernel/core/syscall.c` | Syscall dispatcher and 18 syscall implementations |
| `kernel/mm/paging.c` | Page directory/table management, frame allocator, temp mapping, per-process PDs, page fault handler |
| `kernel/mm/heap.c` | Kernel heap allocator (kmalloc/kfree) |
| `kernel/fs/vfs.c` | In-memory VFS: growable inode table, directories, path resolution, file I/O, dirty tracking |
| `kernel/fs/spikefs.c` | SpikeFS v3: inode chunks in unified data pool, imap chain, full write-back sync/load |
| `kernel/fs/fd.c` | File descriptor subsystem: per-process fd table, open/close/read/write/seek |
| `kernel/fs/pipe.c` | Pipe IPC: circular buffer, blocking read/write |
| `kernel/fs/initrd.c` | Initial ramdisk: parse GRUB module, file lookup, VFS import |
| `kernel/drivers/ata.c` | ATA PIO disk driver (primary master, 28-bit LBA) |
| `kernel/proc/process.c` | Process table, kernel thread + user process creation, fd init, process kill |
| `kernel/proc/scheduler.c` | Round-robin scheduler with CR3 switching (via HAL) |
| `kernel/proc/wait.c` | Wait queue implementation: sleep_on, wake_up_one, wake_up_all |
| `kernel/shell/shell.c` | Kernel shell with command parsing, filesystem commands, test commands, auto write-back |
| `kernel/shell/boot_splash.c` | 1980s-style retro boot splash |
| `kernel/arch/i386/hal.c` | HAL implementation for i386: interrupt, I/O, TLB, MMU wrappers |
| `kernel/arch/i386/isr_stub.S` | ISR/IRQ/syscall assembly stubs, context switch via stack swap |
| `kernel/arch/i386/boot.S` | Multiboot entry, bootstrap paging, jump to higher-half |
| `kernel/arch/i386/linker.ld` | Linker script: physical load at 0x200000, VMA at 0xC0000000+ |
| `scripts/qemu.sh` | Build + run in QEMU (BIOS) |
| `scripts/qemu-uefi.sh` | Build + run in QEMU with UEFI firmware (OVMF/EDK2) |
| `scripts/setup-efi.sh` | One-time setup: symlinks x86_64-efi GRUB modules for hybrid ISO |

## Resources

- [wiki.osdev.org](https://wiki.osdev.org) — x86 OS development wiki
- [OSTEP](https://pages.cs.wisc.edu/~remzi/OSTEP/#book-chapters) — Operating Systems: Three Easy Pieces (University of Wisconsin-Madison)
- *OPERATING SYSTEMS - Design & Implementation* by Andrew S. Tanenbaum
