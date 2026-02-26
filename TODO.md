# SpikeOS TODO

## Automated GUI Testing

Add synthetic mouse/keyboard event injection for scripted GUI tests.

**How the mouse works today:**
- PS/2 mouse on IRQ12 → `mouse_irq()` assembles 3-byte packets → `mouse_process_packet()` updates `mouse_state` (x/y/buttons), redraws cursor, pushes events to ring buffer → `wm_process_events()` polls and dispatches

**What's needed:**
- `mouse_warp(x, y)` — teleport cursor to absolute position, push `EVENT_MOUSE_MOVE`
- `mouse_inject_button(button, pressed)` — simulate click, push `EVENT_MOUSE_BUTTON`
- `keyboard_inject_key(key, ch)` — simulate keypress, push `EVENT_KEY_PRESS`
- Test harness (`test gui` shell command) that runs scripted sequences: warp → delay → click → verify

**Files to touch:** `mouse.c`, `mouse.h`, `keyboard.c`, `keyboard.h`, new `gui_test.c`
