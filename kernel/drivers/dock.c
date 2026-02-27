#include <kernel/dock.h>
#include <kernel/window.h>
#include <kernel/framebuffer.h>
#include <kernel/fb_console.h>
#include <kernel/event.h>
#include <kernel/mouse.h>
#include <kernel/process.h>
#include <kernel/hal.h>
#include <kernel/timer.h>
#include <kernel/shell.h>
#include <kernel/tetris.h>
#include <kernel/gui_editor.h>
#include <kernel/finder.h>
#include <kernel/gl_test.h>
#include <kernel/tty.h>
#include <string.h>
#include <stdio.h>

#define FONT_W 8
#define FONT_H 16

/* ------------------------------------------------------------------ */
/*  Dock app entry                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *name;       /* display label */
    int running;            /* 1 if at least one instance running */
    void (*launch)(void);   /* launch callback */
    void (*draw_icon)(uint32_t cx, uint32_t cy);  /* procedural icon renderer */
} dock_app_t;

/* Forward declarations */
static void dock_launch_shell(void);
static void dock_launch_editor(void);
static void dock_launch_tetris(void);
static void dock_launch_finder(void);
static void dock_launch_opengl(void);
static void draw_icon_shell(uint32_t cx, uint32_t cy);
static void draw_icon_editor(uint32_t cx, uint32_t cy);
static void draw_icon_tetris(uint32_t cx, uint32_t cy);
static void draw_icon_finder(uint32_t cx, uint32_t cy);
static void draw_icon_opengl(uint32_t cx, uint32_t cy);

#define DOCK_APP_COUNT 5

static dock_app_t apps[DOCK_APP_COUNT] = {
    { "Shell",  0, dock_launch_shell,  draw_icon_shell  },
    { "Editor", 0, dock_launch_editor, draw_icon_editor },
    { "Tetris", 0, dock_launch_tetris, draw_icon_tetris },
    { "Finder", 0, dock_launch_finder, draw_icon_finder },
    { "OpenGL", 0, dock_launch_opengl, draw_icon_opengl },
};

/* ------------------------------------------------------------------ */
/*  Dock geometry (computed once in dock_init)                          */
/* ------------------------------------------------------------------ */

static int32_t  pill_x, pill_y;
static uint32_t pill_w, pill_h;
static int      dock_inited = 0;

/* Colors */
static uint32_t pill_bg;
static uint32_t pill_border;
static uint32_t pill_sep;
static uint32_t label_bg;
static uint32_t label_fg;
static uint32_t dot_color;

/* Hover state */
static int hovered_idx = -1;

/* ------------------------------------------------------------------ */
/*  Geometry                                                           */
/* ------------------------------------------------------------------ */

static void dock_calc_geometry(void) {
    uint32_t icons_w = DOCK_APP_COUNT * DOCK_ICON_SIZE +
                       (DOCK_APP_COUNT - 1) * DOCK_ICON_PAD;
    pill_w = icons_w + 2 * DOCK_PILL_PAD_X;
    pill_h = DOCK_ICON_SIZE + 2 * DOCK_PILL_PAD_Y;

    pill_x = (int32_t)(fb_info.width - pill_w) / 2;
    pill_y = (int32_t)(fb_info.height - pill_h - DOCK_MARGIN_BOTTOM);
}

/* Return the center-x of icon at index i */
static uint32_t icon_cx(int i) {
    return (uint32_t)pill_x + DOCK_PILL_PAD_X +
           (uint32_t)i * (DOCK_ICON_SIZE + DOCK_ICON_PAD) +
           DOCK_ICON_SIZE / 2;
}

/* Return the center-y of icon area */
static uint32_t icon_cy(void) {
    return (uint32_t)pill_y + DOCK_PILL_PAD_Y + DOCK_ICON_SIZE / 2;
}

/* Return the left edge of icon at index i */
static uint32_t icon_left(int i) {
    return (uint32_t)pill_x + DOCK_PILL_PAD_X +
           (uint32_t)i * (DOCK_ICON_SIZE + DOCK_ICON_PAD);
}

/* ------------------------------------------------------------------ */
/*  Pill rendering (rounded rectangle)                                 */
/* ------------------------------------------------------------------ */

static void dock_draw_pill(void) {
    uint32_t x = (uint32_t)pill_x;
    uint32_t y = (uint32_t)pill_y;
    uint32_t w = pill_w;
    uint32_t h = pill_h;
    uint32_t r = DOCK_CORNER_R;

    uint32_t desktop = wm_get_desktop_color();

    /* Central rectangle (full width, excluding corner rows) */
    fb_fill_rect(x, y + r, w, h - 2 * r, pill_bg);

    /* Top strip (between corners) */
    fb_fill_rect(x + r, y, w - 2 * r, r, pill_bg);

    /* Bottom strip (between corners) */
    fb_fill_rect(x + r, y + h - r, w - 2 * r, r, pill_bg);

    /* Four anti-aliased corner circles */
    fb_fill_circle_aa(x + r,     y + r,     r, pill_bg, desktop);  /* TL */
    fb_fill_circle_aa(x + w - r - 1, y + r,     r, pill_bg, desktop);  /* TR */
    fb_fill_circle_aa(x + r,     y + h - r - 1, r, pill_bg, desktop);  /* BL */
    fb_fill_circle_aa(x + w - r - 1, y + h - r - 1, r, pill_bg, desktop);  /* BR */

    /* Top border line (subtle highlight) */
    fb_draw_hline(x + r, y, w - 2 * r, pill_border);
}

/* ------------------------------------------------------------------ */
/*  Procedural icons                                                   */
/* ------------------------------------------------------------------ */

static void draw_icon_shell(uint32_t cx, uint32_t cy) {
    /* Terminal window: dark rounded body with >_ prompt */
    uint32_t bx = cx - 18, by = cy - 18;
    uint32_t body_bg = fb_pack_color(30, 30, 38);
    uint32_t border  = fb_pack_color(60, 60, 70);
    uint32_t bar_bg  = fb_pack_color(50, 50, 60);
    uint32_t green   = fb_pack_color(80, 220, 100);

    /* Body */
    fb_fill_rect(bx, by, 36, 36, body_bg);
    fb_draw_rect(bx, by, 36, 36, border);

    /* Title bar strip */
    fb_fill_rect(bx + 1, by + 1, 34, 7, bar_bg);
    fb_draw_hline(bx + 1, by + 8, 34, border);

    /* Traffic light dots (tiny) */
    fb_fill_circle_aa(bx + 6,  by + 5, 2, fb_pack_color(255, 95, 86), bar_bg);
    fb_fill_circle_aa(bx + 12, by + 5, 2, fb_pack_color(255, 189, 46), bar_bg);
    fb_fill_circle_aa(bx + 18, by + 5, 2, fb_pack_color(39, 201, 63), bar_bg);

    /* Prompt: >_ */
    fb_render_char_px(bx + 6,  by + 14, '>', green, body_bg);
    fb_render_char_px(bx + 14, by + 14, '_', green, body_bg);
}

static void draw_icon_editor(uint32_t cx, uint32_t cy) {
    /* Document page with text lines */
    uint32_t px = cx - 14, py = cy - 18;
    uint32_t page_bg  = fb_pack_color(240, 240, 245);
    uint32_t page_brd = fb_pack_color(180, 180, 190);
    uint32_t line_clr = fb_pack_color(140, 150, 170);
    uint32_t blue     = fb_pack_color(70, 130, 210);
    uint32_t fold_clr = fb_pack_color(200, 200, 210);

    /* Page body */
    fb_fill_rect(px, py, 28, 36, page_bg);
    fb_draw_rect(px, py, 28, 36, page_brd);

    /* Dog-ear at top-right */
    fb_fill_rect(px + 20, py, 8, 8, fold_clr);
    fb_draw_hline(px + 20, py + 8, 8, page_brd);
    fb_draw_vline(px + 20, py, 8, page_brd);

    /* Blue header line */
    fb_fill_rect(px + 4, py + 6, 16, 2, blue);

    /* Text lines */
    fb_fill_rect(px + 4, py + 12, 20, 2, line_clr);
    fb_fill_rect(px + 4, py + 17, 14, 2, line_clr);
    fb_fill_rect(px + 4, py + 22, 18, 2, line_clr);
    fb_fill_rect(px + 4, py + 27, 10, 2, line_clr);
}

static void draw_icon_tetris(uint32_t cx, uint32_t cy) {
    /* Tetris blocks on dark background */
    uint32_t bx = cx - 18, by = cy - 18;
    uint32_t dark = fb_pack_color(20, 20, 30);
    uint32_t grid = fb_pack_color(35, 35, 45);

    /* Background */
    fb_fill_rect(bx, by, 36, 36, dark);
    fb_draw_rect(bx, by, 36, 36, fb_pack_color(50, 50, 60));

    /* Block colors (standard Tetris palette) */
    uint32_t cyan    = fb_pack_color(0, 220, 220);
    uint32_t yellow  = fb_pack_color(220, 220, 0);
    uint32_t magenta = fb_pack_color(180, 0, 220);
    uint32_t green   = fb_pack_color(0, 220, 80);
    uint32_t red     = fb_pack_color(220, 40, 40);
    uint32_t blue    = fb_pack_color(40, 80, 220);
    uint32_t orange  = fb_pack_color(220, 140, 0);

    /* Block size: 8×8 with 1px gap, arranged in a mixed pattern */
    #define BLK 8
    #define BG  1  /* gap between blocks */

    /* Row 0 (top area) */
    fb_fill_rect(bx + 4,          by + 4,  BLK, BLK, cyan);
    fb_fill_rect(bx + 4 + BLK+BG, by + 4,  BLK, BLK, cyan);
    fb_fill_rect(bx + 4 + 2*(BLK+BG), by + 4, BLK, BLK, cyan);

    /* Row 1 */
    fb_fill_rect(bx + 4,          by + 4 + BLK+BG, BLK, BLK, magenta);
    fb_fill_rect(bx + 4 + BLK+BG, by + 4 + BLK+BG, BLK, BLK, yellow);
    fb_fill_rect(bx + 4 + 2*(BLK+BG), by + 4 + BLK+BG, BLK, BLK, green);

    /* Row 2 */
    fb_fill_rect(bx + 4,          by + 4 + 2*(BLK+BG), BLK, BLK, red);
    fb_fill_rect(bx + 4 + BLK+BG, by + 4 + 2*(BLK+BG), BLK, BLK, blue);
    fb_fill_rect(bx + 4 + 2*(BLK+BG), by + 4 + 2*(BLK+BG), BLK, BLK, orange);

    #undef BLK
    #undef BG

    (void)grid;
}

static void draw_icon_finder(uint32_t cx, uint32_t cy) {
    /* Blue folder with tab */
    uint32_t bx = cx - 16, by = cy - 14;
    uint32_t folder_bg = fb_pack_color(80, 140, 220);
    uint32_t folder_dk = fb_pack_color(60, 110, 190);
    uint32_t tab_bg    = fb_pack_color(100, 160, 240);

    /* Folder tab (top-left) */
    fb_fill_rect(bx, by, 14, 6, tab_bg);
    /* Folder body */
    fb_fill_rect(bx, by + 6, 32, 22, folder_bg);
    fb_draw_rect(bx, by + 6, 32, 22, folder_dk);
    /* Fold line */
    fb_draw_hline(bx + 1, by + 12, 30, folder_dk);
}

static void draw_icon_opengl(uint32_t cx, uint32_t cy) {
    /* 3D cube/tetrahedron on dark background — represents OpenGL */
    uint32_t bx = cx - 18, by = cy - 18;
    uint32_t dark   = fb_pack_color(15, 15, 25);
    uint32_t border = fb_pack_color(40, 40, 55);

    /* Background */
    fb_fill_rect(bx, by, 36, 36, dark);
    fb_draw_rect(bx, by, 36, 36, border);

    /* Red triangle (front face) */
    uint32_t red   = fb_pack_color(220, 40, 40);
    uint32_t green = fb_pack_color(40, 200, 60);
    uint32_t blue  = fb_pack_color(50, 80, 220);

    /* Draw a filled triangle using horizontal lines:
     * Top vertex at (cx, by+5), bottom-left (bx+5, by+30), bottom-right (bx+31, by+30) */
    {
        int top_x = (int)cx, top_y = (int)by + 5;
        int bl_x = (int)bx + 5, br_x = (int)bx + 31;
        int bot_y = (int)by + 30;
        int tri_h = bot_y - top_y;

        for (int row = 0; row < tri_h; row++) {
            int y = top_y + row;
            float t = (float)row / (float)tri_h;
            int lx = top_x + (int)((float)(bl_x - top_x) * t);
            int rx = top_x + (int)((float)(br_x - top_x) * t);
            if (lx > rx) { int tmp = lx; lx = rx; rx = tmp; }

            /* Color gradient: red at top, green bottom-left, blue bottom-right */
            uint8_t r = (uint8_t)(220 - (int)(180.0f * t));
            uint8_t g = (uint8_t)(40 + (int)(160.0f * t));
            uint8_t b = (uint8_t)(40 + (int)(80.0f * t));
            uint32_t clr = fb_pack_color(r, g, b);

            if (rx > lx)
                fb_fill_rect((uint32_t)lx, (uint32_t)y,
                             (uint32_t)(rx - lx), 1, clr);
        }
    }

    /* "GL" text overlay at bottom */
    fb_render_char_px(bx + 10, by + 20, 'G', green, dark);
    fb_render_char_px(bx + 18, by + 20, 'L', blue, dark);

    (void)red;
}

/* ------------------------------------------------------------------ */
/*  Hover tooltip                                                      */
/* ------------------------------------------------------------------ */

static void dock_draw_tooltip(int idx) {
    if (idx < 0 || idx >= DOCK_APP_COUNT) return;

    const char *name = apps[idx].name;
    int len = 0;
    while (name[len]) len++;

    uint32_t tw = (uint32_t)len * FONT_W + 12;  /* 6px padding each side */
    uint32_t th = FONT_H + 6;                    /* 3px padding top+bottom */
    uint32_t tx = icon_cx(idx) - tw / 2;
    uint32_t ty = (uint32_t)pill_y - th - 6;

    /* Tooltip background */
    fb_fill_rect(tx, ty, tw, th, label_bg);

    /* Text */
    uint32_t text_x = tx + 6;
    uint32_t text_y = ty + 3;
    for (int i = 0; i < len; i++) {
        fb_render_char_px(text_x + (uint32_t)i * FONT_W, text_y,
                          (uint8_t)name[i], label_fg, label_bg);
    }
}

/* ------------------------------------------------------------------ */
/*  Running indicator dots                                             */
/* ------------------------------------------------------------------ */

static void dock_draw_dots(void) {
    for (int i = 0; i < DOCK_APP_COUNT; i++) {
        if (apps[i].running) {
            uint32_t dcx = icon_cx(i);
            uint32_t dcy = (uint32_t)pill_y + pill_h - 5;
            fb_fill_circle_aa(dcx, dcy, 2, dot_color, pill_bg);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Public API: init / draw / click / hover / height                   */
/* ------------------------------------------------------------------ */

void dock_init(void) {
    if (!fb_info.available) return;

    pill_bg     = fb_pack_color(40, 40, 50);
    pill_border = fb_pack_color(80, 80, 90);
    pill_sep    = fb_pack_color(70, 70, 85);
    label_bg    = fb_pack_color(30, 30, 40);
    label_fg    = fb_pack_color(230, 230, 230);
    dot_color   = fb_pack_color(255, 255, 255);

    dock_calc_geometry();
    dock_inited = 1;
}

void dock_draw(void) {
    if (!dock_inited) return;

    /* Erase tooltip area above the pill (in case hover changed) */
    uint32_t tooltip_h = FONT_H + 12;  /* tooltip height + gap */
    uint32_t tooltip_y = (uint32_t)pill_y - tooltip_h;
    fb_fill_rect((uint32_t)pill_x, tooltip_y, pill_w, tooltip_h,
                 wm_get_desktop_color());

    /* Draw the pill background */
    dock_draw_pill();

    /* Draw icons */
    uint32_t icy = icon_cy();
    for (int i = 0; i < DOCK_APP_COUNT; i++) {
        apps[i].draw_icon(icon_cx(i), icy);
    }

    /* Draw separator lines between icons */
    for (int i = 0; i < DOCK_APP_COUNT - 1; i++) {
        uint32_t sep_x = icon_left(i) + DOCK_ICON_SIZE + DOCK_ICON_PAD / 2;
        uint32_t sep_y1 = (uint32_t)pill_y + DOCK_PILL_PAD_Y + 8;
        uint32_t sep_h  = DOCK_ICON_SIZE - 16;
        fb_draw_vline(sep_x, sep_y1, sep_h, pill_sep);
    }

    /* Draw running indicator dots */
    dock_draw_dots();

    /* Draw tooltip if hovering */
    if (hovered_idx >= 0)
        dock_draw_tooltip(hovered_idx);
}

int dock_click(int32_t mx, int32_t my) {
    if (!dock_inited) return 0;

    /* Check if click is within the pill bounds */
    if (mx < pill_x || mx >= pill_x + (int32_t)pill_w ||
        my < pill_y || my >= pill_y + (int32_t)pill_h)
        return 0;

    /* Determine which icon was clicked */
    for (int i = 0; i < DOCK_APP_COUNT; i++) {
        int32_t il = (int32_t)icon_left(i);
        int32_t ir = il + DOCK_ICON_SIZE;
        if (mx >= il && mx < ir) {
            apps[i].launch();
            return 1;
        }
    }

    return 1;  /* consumed (click in pill padding) */
}

void dock_hover(int32_t mx, int32_t my) {
    if (!dock_inited) return;

    int old_hover = hovered_idx;
    hovered_idx = -1;

    if (mx >= pill_x && mx < pill_x + (int32_t)pill_w &&
        my >= pill_y && my < pill_y + (int32_t)pill_h) {
        for (int i = 0; i < DOCK_APP_COUNT; i++) {
            int32_t il = (int32_t)icon_left(i);
            int32_t ir = il + DOCK_ICON_SIZE;
            if (mx >= il && mx < ir) {
                hovered_idx = i;
                break;
            }
        }
    }

    if (hovered_idx != old_hover) {
        wm_redraw_all();
    }
}

uint32_t dock_reserved_height(void) {
    if (!dock_inited) return 0;
    return pill_h + DOCK_MARGIN_BOTTOM + 4;  /* pill + margin + small gap */
}

/* ------------------------------------------------------------------ */
/*  Running state tracking                                             */
/* ------------------------------------------------------------------ */

void dock_update_running(void) {
    if (!dock_inited) return;

    /* Shell: check if shell window exists and is valid */
    apps[0].running = (wm_get_shell_window() != NULL) ? 1 : 0;

    /* Editor, Tetris, Finder, OpenGL: scan window list by title */
    apps[1].running = 0;
    apps[2].running = 0;
    apps[3].running = 0;
    apps[4].running = 0;

    /* Walk the z-order list via wm_get_top_window and traverse prev.
       We can't access win_bottom directly, so use the top window and walk down. */
    window_t *w = wm_get_top_window();
    while (w) {
        if (w->flags & WIN_FLAG_VISIBLE) {
            /* Check for editor: title starts with "Edit: " */
            if (w->title[0] == 'E' && w->title[1] == 'd' &&
                w->title[2] == 'i' && w->title[3] == 't' &&
                w->title[4] == ':')
                apps[1].running = 1;

            /* Check for Tetris */
            if (w->title[0] == 'T' && w->title[1] == 'e' &&
                w->title[2] == 't' && w->title[3] == 'r' &&
                w->title[4] == 'i' && w->title[5] == 's')
                apps[2].running = 1;

            /* Check for Finder */
            if (w->title[0] == 'F' && w->title[1] == 'i' &&
                w->title[2] == 'n' && w->title[3] == 'd' &&
                w->title[4] == 'e' && w->title[5] == 'r')
                apps[3].running = 1;

            /* Check for OpenGL Demo */
            if (w->title[0] == 'O' && w->title[1] == 'p' &&
                w->title[2] == 'e' && w->title[3] == 'n' &&
                w->title[4] == 'G' && w->title[5] == 'L')
                apps[4].running = 1;
        }
        w = w->prev;
    }
}

/* ------------------------------------------------------------------ */
/*  Launch callbacks                                                   */
/* ------------------------------------------------------------------ */

static void dock_launch_shell(void) {
    /* If shell window exists, just focus it */
    window_t *existing = wm_get_shell_window();
    if (existing) {
        existing->flags |= WIN_FLAG_VISIBLE;  /* un-minimize if needed */
        wm_focus_window(existing);
        wm_redraw_all();
        return;
    }

    /* Create shell window — 80% width, 50% height, positioned near bottom */
    uint32_t content_w = (fb_info.width * 4 / 5);
    uint32_t content_h = (fb_info.height / 2);
    content_w = (content_w / 8) * 8;   /* align to font grid */
    content_h = (content_h / 16) * 16;

    uint32_t outer_w = content_w + 2 * WIN_BORDER_W;
    uint32_t outer_h = content_h + WIN_TITLEBAR_H + 2 * WIN_BORDER_W;
    int32_t  outer_x = ((int32_t)fb_info.width - (int32_t)outer_w) / 2;
    int32_t  outer_y = (int32_t)fb_info.height - (int32_t)outer_h -
                       (int32_t)dock_reserved_height() - 16;
    if (outer_y < (int32_t)WM_DESKBAR_H)
        outer_y = (int32_t)WM_DESKBAR_H;

    window_t *win = wm_create_window(outer_x, outer_y, outer_w, outer_h,
                                      "SpikeOS Shell");
    if (!win) return;

    wm_set_shell_window(win);
    fb_console_bind_window(win);
    fb_console_setcolor(14, 0);  /* yellow on black */
    fb_console_clear();
    wm_redraw_all();

    /* Spawn shell thread */
    proc_create_kernel_thread(shell_run);
}

static void dock_launch_editor(void) {
    gui_editor_open("/untitled");
}

/* Wrapper so tetris_run() can execute in its own kernel thread */
static void tetris_thread_wrapper(void) {
    tetris_run();
    dock_update_running();
    proc_kill(current_process->pid);
    for (;;) __asm__ volatile("hlt");
}

static void dock_launch_tetris(void) {
    proc_create_kernel_thread(tetris_thread_wrapper);
}

static void dock_launch_finder(void) {
    finder_open("/");
}

/* Wrapper so gl_test_run() can execute in its own kernel thread */
static void opengl_thread_wrapper(void) {
    gl_test_run();
    dock_update_running();
    proc_kill(current_process->pid);
    for (;;) __asm__ volatile("hlt");
}

static void dock_launch_opengl(void) {
    proc_create_kernel_thread(opengl_thread_wrapper);
}

/* ------------------------------------------------------------------ */
/*  Desktop event loop                                                 */
/* ------------------------------------------------------------------ */

void dock_desktop_loop(void) {
    while (1) {
        wm_process_events();
        __asm__ volatile("hlt");
    }
}
