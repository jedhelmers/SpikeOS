#ifndef _WINDOW_H
#define _WINDOW_H

#include <stdint.h>

#define WIN_MAX_TITLE    32
#define WIN_TITLEBAR_H   20   /* title bar height in pixels */
#define WIN_BORDER_W      1   /* border thickness in pixels */

/* Window flags */
#define WIN_FLAG_VISIBLE   (1 << 0)
#define WIN_FLAG_FOCUSED   (1 << 1)
#define WIN_FLAG_DRAGGABLE (1 << 2)
#define WIN_FLAG_DRAGGING  (1 << 3)

typedef struct window {
    /* Outer frame position and size (pixels) */
    int32_t  x, y;
    uint32_t w, h;

    /* Content area (derived — call wm_update_content_rect to refresh) */
    uint32_t content_x, content_y;
    uint32_t content_w, content_h;

    /* Appearance */
    char     title[WIN_MAX_TITLE];
    uint32_t title_bg_color;
    uint32_t title_fg_color;
    uint32_t body_bg_color;
    uint32_t border_color;

    /* State */
    uint32_t flags;

    /* Drag tracking */
    int32_t  drag_off_x, drag_off_y;

    /* Window list — bottom to top z-order (next = above, prev = below) */
    struct window *next;
    struct window *prev;
} window_t;

/* Initialize window manager (call after fb_enable) */
void wm_init(void);

/* Create a window (heap-allocated). */
window_t *wm_create_window(int32_t x, int32_t y, uint32_t w, uint32_t h,
                            const char *title);

/* Recompute content rect from outer geometry */
void wm_update_content_rect(window_t *win);

/* Draw window chrome (border + title bar). Does not touch content area. */
void wm_draw_chrome(window_t *win);

/* Fill entire desktop background */
void wm_draw_desktop(void);

/* Redraw everything: desktop + all windows' chrome + content */
void wm_redraw_all(void);

/* Poll and process one WM event (mouse drag, etc).
   Returns 1 if an event was consumed, 0 otherwise. */
int wm_process_events(void);

/* Refresh desktop (redraw background + icons + window on top) */
void wm_refresh_desktop(void);

/* Bring a window to the front of the z-order and set focus */
void wm_focus_window(window_t *win);

/* Find the topmost window at screen coordinates (mx, my), or NULL */
window_t *wm_window_at(int32_t mx, int32_t my);

/* Accessors */
window_t *wm_get_shell_window(void);
window_t *wm_get_top_window(void);
uint32_t  wm_get_desktop_color(void);

#endif
