# kernel/shell/

User interface for SpikeOS.

## What's Here

- **shell.c** — Interactive kernel shell with filesystem commands, process control, test suite, and concurrent mutex demo
- **boot_splash.c** — 1980s retro boot animation with ASCII art logo and progress bar
- **tetris.c** — Tetris game in VGA mode 13h (320x200, 256-color)

## How It Fits Together

The shell runs as a kernel thread started by `kernel_main()`. It provides a command-line interface for filesystem operations (ls, cd, cat, write, etc.), process management (ps, kill, run, exec), and kernel testing (test all).

The `run concurrent` command demonstrates mutex-protected shared state between two threads. `run tetris` switches to VGA graphics mode for the game. The boot splash runs during kernel init (before interrupts are enabled) using busy-wait timing.

Type `help` at the shell prompt for a full command list.
