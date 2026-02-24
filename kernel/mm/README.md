# kernel/mm/

Memory management for SpikeOS.

## What's Here

- **paging.c** — Two-level page tables (PD + PT), physical frame allocator (bitmap, 64MB), per-process page directories, interrupt-safe temporary mapping window, page fault handler
- **heap.c** — Kernel heap allocator: first-fit free-list with splitting and coalescing (`kmalloc`/`kfree`/`kcalloc`/`krealloc`), interrupt-safe via `hal_irq_save/restore`

## How It Fits Together

`paging_init()` sets up the kernel's page directory with identity mapping (first 4MB), higher-half mapping (0xC0000000+), and heap mapping (0xC0400000+). Each user process gets its own page directory via `pgdir_create()`, which clones the kernel mappings and adds user-space pages.

`map_page()` and `virt_to_phys()` use `temp_map()` to safely access page tables by physical address, rather than assuming physical addresses are valid virtual pointers. `alloc_frame()` returns `FRAME_ALLOC_FAIL` (0xFFFFFFFF) on OOM, and `map_page()` returns -1 on failure — all callers check for errors.

The heap provides dynamic allocation for all kernel subsystems (VFS inodes, file data, process stacks, etc.). It starts at 4MB in the kernel's virtual space and grows on demand by mapping new physical frames. `heap_grow()` includes partial-failure rollback if frame allocation fails mid-grow.

`temp_map()`/`temp_unmap()` use `hal_irq_save/restore` for interrupt safety — only one temp mapping slot exists (PTE[1023]), so ISRs must not re-enter while a mapping is active.
