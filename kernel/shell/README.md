# kernel/shell/

User interface for SpikeOS — shell, text editors, file manager, game, and boot animation.

## What's Here

- **shell.c** — Interactive kernel shell with filesystem commands, process control, test suite, and concurrent mutex demo
- **editor.c** — Nano-like shell text editor (runs in the shell window, invoked via `edit <filename>`)
- **gui_editor.c** — GUI windowed text editor with toolbar, font scaling, word wrap, selection/clipboard, and undo/redo
- **finder.c** — Finder file manager with column view, sidebar, scrollbar, inline rename, and right-click context menus
- **tetris.c** — Tetris game in a framebuffer window (16px cells, cooperative close via `WIN_FLAG_CLOSE_REQ`)
- **boot_splash.c** — 1980s retro boot animation with ASCII art logo and progress bar

## How It Fits Together

The shell runs as a kernel thread started by `kernel_main()`. It provides a command-line interface for filesystem operations (ls, cd, cat, write, etc.), process management (ps, kill, run, exec), and kernel testing (test all). Mouse scroll wheel drives shell history scrollback via `terminal_scroll_lines()`.

The shell text editor (`edit`) takes over the shell window for inline file editing. The GUI editor and Finder each spawn as separate kernel threads in their own framebuffer windows — opened from the dock, desktop icons, or shell commands. Up to 4 GUI editor instances and multiple Finder windows can run concurrently.

`run tetris` opens a framebuffer window for the game. The boot splash runs during kernel init (before interrupts are enabled) using busy-wait timing.

Type `help` at the shell prompt for a full command list.
