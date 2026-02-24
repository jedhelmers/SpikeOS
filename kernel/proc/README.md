# kernel/proc/

Process management, scheduling, and synchronization for SpikeOS.

## What's Here

- **process.c** — Process table (max 32 processes), kernel thread and user process creation, interrupt-safe kill (switches to kernel CR3 before destroying user PD), signal delivery with zombie guard
- **scheduler.c** — Round-robin preemptive scheduler with CR3 switching, NULL guard for premature timer IRQ, idle fallback in `pick_next()`
- **elf_loader.c** — ELF binary loader for user-mode processes (VFS first, initrd fallback)
- **wait.c** — Wait queues for blocking/waking processes
- **mutex.c** — Spinlock, blocking mutex, and counting semaphore
- **condvar.c** — Condition variables (wait/signal/broadcast)
- **rwlock.c** — Reader-writer locks with writer starvation prevention

## How It Fits Together

The process table holds up to 32 processes. The scheduler runs round-robin at 100Hz (timer IRQ), switching between READY processes by saving/restoring kernel stack pointers and switching CR3 for per-process page directories.

Wait queues are the foundation for all blocking primitives: mutex, semaphore, condition variable, rwlock, and pipe I/O. A process calls `sleep_on()` to block, and another process or IRQ handler calls `wake_up_one()`/`wake_up_all()` to unblock.

The ELF loader creates user-mode processes with their own page directories, mapping code/data segments and a user stack at the top of user address space.
