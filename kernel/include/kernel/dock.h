#ifndef _DOCK_H
#define _DOCK_H

#include <stdint.h>

/* Dock layout constants */
#define DOCK_ICON_SIZE      48   /* icon drawing area (pixels) */
#define DOCK_ICON_PAD       16   /* padding between icons */
#define DOCK_PILL_PAD_X     20   /* horizontal padding inside pill */
#define DOCK_PILL_PAD_Y     10   /* vertical padding inside pill */
#define DOCK_MARGIN_BOTTOM   8   /* gap from bottom of screen */
#define DOCK_CORNER_R       16   /* pill corner radius */
#define DOCK_MAX_APPS        8   /* maximum dock entries */

/* Initialize dock state and calculate geometry (call after wm_init) */
void dock_init(void);

/* Draw the dock pill, icons, labels, and running dots to the framebuffer */
void dock_draw(void);

/* Handle a mouse click at (mx, my). Returns 1 if consumed, 0 otherwise. */
int dock_click(int32_t mx, int32_t my);

/* Track mouse hover for tooltip label display. Call on MOUSE_MOVE events. */
void dock_hover(int32_t mx, int32_t my);

/* Returns the height in pixels reserved at the bottom of the screen
   (used by maximize clamping). */
uint32_t dock_reserved_height(void);

/* Scan the window list to update running indicators. Call after
   wm_create_window() and wm_destroy_window(). */
void dock_update_running(void);

/* Main desktop event loop — replaces shell_run() in kernel_main().
   Never returns. */
void dock_desktop_loop(void);

#endif
