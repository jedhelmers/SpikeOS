# kernel/mm/

Memory management for SpikeOS.

## What's Here

- **paging.c** — Two-level page tables (PD + PT), physical frame allocator (bitmap, 64MB), per-process page directories, temporary mapping window
- **heap.c** — Kernel heap allocator: first-fit free-list with splitting and coalescing (`kmalloc`/`kfree`/`kcalloc`/`krealloc`)

## How It Fits Together

`paging_init()` sets up the kernel's page directory with identity mapping (first 4MB), higher-half mapping (0xC0000000+), and heap mapping (0xC0400000+). Each user process gets its own page directory via `pgdir_create()`, which clones the kernel mappings and adds user-space pages.

The heap provides dynamic allocation for all kernel subsystems (VFS inodes, file data, process stacks, etc.). It starts at 4MB in the kernel's virtual space and grows on demand by mapping new physical frames.
