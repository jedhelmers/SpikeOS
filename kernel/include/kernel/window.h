#ifndef _WINDOW_H
#define _WINDOW_H

#include <stdint.h>

#define WIN_MAX_TITLE    32
#define WIN_TITLEBAR_H   20   /* title bar height in pixels */
#define WIN_BORDER_W      1   /* border thickness in pixels */
#define WIN_RESIZE_GRIP   6   /* resize grip zone in pixels */
#define WIN_MIN_W       120   /* minimum window width */
#define WIN_MIN_H        80   /* minimum window height */
#define WIN_BORDER_RADIUS  4  /* corner radius in pixels */

/* Traffic light dot layout (relative to window top-left) */
#define WIN_DOT_RADIUS     5
#define WIN_DOT_Y_OFF     10  /* center Y offset from window top */
#define WIN_DOT_CLOSE_X   14  /* close dot center X */
#define WIN_DOT_MIN_X     30  /* minimize dot center X */
#define WIN_DOT_MAX_X     46  /* maximize dot center X */

/* Window flags */
#define WIN_FLAG_VISIBLE    (1 << 0)
#define WIN_FLAG_FOCUSED    (1 << 1)
#define WIN_FLAG_DRAGGABLE  (1 << 2)
#define WIN_FLAG_DRAGGING   (1 << 3)
#define WIN_FLAG_RESIZABLE  (1 << 4)
#define WIN_FLAG_RESIZING   (1 << 5)
#define WIN_FLAG_MAXIMIZED  (1 << 6)
#define WIN_FLAG_CLOSE_REQ  (1 << 7)  /* close dot was clicked; owner should clean up */

/* Resize edge mask (which edges are being dragged) */
#define RESIZE_LEFT    (1 << 0)
#define RESIZE_RIGHT   (1 << 1)
#define RESIZE_TOP     (1 << 2)
#define RESIZE_BOTTOM  (1 << 3)

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

    /* Resize tracking */
    uint32_t resize_edges;   /* RESIZE_LEFT/RIGHT/TOP/BOTTOM mask */
    int32_t  resize_anchor_x, resize_anchor_y;
    int32_t  resize_orig_x, resize_orig_y;
    uint32_t resize_orig_w, resize_orig_h;

    /* Saved geometry for maximize/restore toggle */
    int32_t  saved_x, saved_y;
    uint32_t saved_w, saved_h;

    /* Window list — bottom to top z-order (next = above, prev = below) */
    struct window *next;
    struct window *prev;
} window_t;

/* Initialize window manager (call after fb_enable) */
void wm_init(void);

/* Create a window (heap-allocated). */
window_t *wm_create_window(int32_t x, int32_t y, uint32_t w, uint32_t h, const char *title);

/* Destroy a window: unlink from z-order, free memory, redraw desktop. */
void wm_destroy_window(window_t *win);

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
