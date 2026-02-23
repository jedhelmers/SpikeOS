# kernel/fs/

File system layer for SpikeOS.

## What's Here

- **vfs.c** — In-memory Virtual File System with growable inode table, directory entries, path resolution, and dirty tracking
- **spikefs.c** — On-disk filesystem (v3) with unified data pool, inode map chain, and full write-back sync
- **initrd.c** — Initial ramdisk parser (GRUB module); files imported into VFS at boot
- **fd.c** — File descriptor subsystem: per-process fd table (16 slots), system-wide open file table (64 slots)
- **pipe.c** — Kernel pipes with 512-byte circular buffer and blocking read/write

## How It Fits Together

The VFS is the central in-memory filesystem. All file operations go through VFS inodes. SpikeFS persists the VFS to disk via full write-back (auto-synced every 5 seconds). The initrd provides initial files from the GRUB boot module. File descriptors multiplex access to VFS files, the console (stdin/stdout/stderr), and pipes. Syscalls in `kernel/core/syscall.c` are the user-space entry point to all file operations.
