#ifndef _MOUSE_H
#define _MOUSE_H

#include <stdint.h>

/* Mouse button bit flags */
#define MOUSE_BTN_LEFT   (1 << 0)
#define MOUSE_BTN_RIGHT  (1 << 1)
#define MOUSE_BTN_MIDDLE (1 << 2)

/* Mouse state — readable at any time */
typedef struct {
    int32_t  x, y;       /* absolute cursor position (pixels) */
    uint8_t  buttons;    /* bitmask of currently held buttons */
} mouse_state_t;

/* Initialize PS/2 mouse, register IRQ12 handler.
   No-op if framebuffer is not available. */
void mouse_init(void);

/* Get current mouse state (non-blocking) */
mouse_state_t mouse_get_state(void);

/* Show/hide the cursor on the framebuffer */
void mouse_show_cursor(void);
void mouse_hide_cursor(void);

/* Redraw cursor at current position (called internally by IRQ12 handler) */
void mouse_update_cursor(void);

#endif
