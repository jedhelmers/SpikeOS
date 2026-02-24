#include <kernel/window.h>
#include <kernel/framebuffer.h>
#include <kernel/fb_console.h>
#include <kernel/event.h>
#include <kernel/mouse.h>
#include <kernel/heap.h>
#include <kernel/vfs.h>
#include <string.h>

#define FONT_W 8
#define FONT_H 16

static uint32_t desktop_color;
static window_t *shell_win = NULL;

/* Desktop icon constants */
#define DESKTOP_PATH "/Users/jedhelmers/Desktop"
#define ICON_W       30
#define ICON_H       50
#define ICON_PAD_X   10
#define ICON_PAD_Y   10
#define ICON_RECT_W  22
#define ICON_RECT_H  28
#define ICON_MAX_LABEL 4

static uint32_t desktop_dir_ino = 0;
static int desktop_dir_valid = 0;

/* ------------------------------------------------------------------ */
/*  Content rect                                                       */
/* ------------------------------------------------------------------ */

void wm_update_content_rect(window_t *win) {
    win->content_x = (uint32_t)win->x + WIN_BORDER_W;
    win->content_y = (uint32_t)win->y + WIN_TITLEBAR_H + WIN_BORDER_W;
    win->content_w = win->w - 2 * WIN_BORDER_W;
    win->content_h = win->h - WIN_TITLEBAR_H - 2 * WIN_BORDER_W;

    /* Snap content to character grid */
    win->content_w = (win->content_w / FONT_W) * FONT_W;
    win->content_h = (win->content_h / FONT_H) * FONT_H;
}

/* ------------------------------------------------------------------ */
/*  Desktop directory setup                                            */
/* ------------------------------------------------------------------ */

static void desktop_ensure_path(void) {
    if (vfs_resolve("/Users", NULL, NULL) < 0)
        vfs_mkdir("/Users");
    if (vfs_resolve("/Users/jedhelmers", NULL, NULL) < 0)
        vfs_mkdir("/Users/jedhelmers");
    if (vfs_resolve(DESKTOP_PATH, NULL, NULL) < 0)
        vfs_mkdir(DESKTOP_PATH);

    int32_t ino = vfs_resolve(DESKTOP_PATH, NULL, NULL);
    if (ino >= 0) {
        desktop_dir_ino = (uint32_t)ino;
        desktop_dir_valid = 1;
    }
}

/* ------------------------------------------------------------------ */
/*  Desktop icons                                                      */
/* ------------------------------------------------------------------ */

static void wm_draw_desktop_icons(void) {
    if (!desktop_dir_valid) return;

    vfs_inode_t *dir = vfs_get_inode(desktop_dir_ino);
    if (!dir || dir->type != VFS_TYPE_DIR) return;

    vfs_dirent_t *entries = (vfs_dirent_t *)dir->data;
    uint32_t count = dir->size;

    uint32_t cell_w = ICON_W + ICON_PAD_X;
    uint32_t cell_h = ICON_H + ICON_PAD_Y;
    uint32_t max_rows = (fb_info.height - ICON_PAD_Y) / cell_h;
    if (max_rows == 0) max_rows = 1;

    uint32_t icon_idx = 0;
    uint32_t file_color = fb_pack_color(100, 140, 200);
    uint32_t dir_color  = fb_pack_color(200, 180, 100);
    uint32_t outline    = fb_pack_color(200, 200, 200);
    uint32_t label_fg   = fb_pack_color(220, 220, 220);

    for (uint32_t i = 0; i < count; i++) {
        /* Skip . and .. */
        if (entries[i].name[0] == '.' &&
            (entries[i].name[1] == '\0' ||
             (entries[i].name[1] == '.' && entries[i].name[2] == '\0')))
            continue;

        uint32_t col = icon_idx / max_rows;
        uint32_t row = icon_idx % max_rows;

        uint32_t cx = ICON_PAD_X + col * cell_w;
        uint32_t cy = ICON_PAD_Y + row * cell_h;

        /* Determine type */
        vfs_inode_t *child = vfs_get_inode(entries[i].inode);
        uint32_t fill = (child && child->type == VFS_TYPE_DIR)
                        ? dir_color : file_color;

        /* Draw icon rect (centered horizontally in cell) */
        uint32_t rx = cx + (ICON_W - ICON_RECT_W) / 2;
        uint32_t ry = cy;
        fb_fill_rect(rx, ry, ICON_RECT_W, ICON_RECT_H, fill);
        fb_draw_rect(rx, ry, ICON_RECT_W, ICON_RECT_H, outline);

        /* Draw filename label below rect */
        uint32_t label_y = ry + ICON_RECT_H + 2;
        int len = 0;
        while (entries[i].name[len] && len < ICON_MAX_LABEL) len++;

        /* Center label under icon */
        uint32_t label_w = (uint32_t)len * FONT_W;
        uint32_t label_x = cx + (ICON_W - label_w) / 2;

        for (int j = 0; j < len; j++) {
            fb_render_char_px(label_x + (uint32_t)j * FONT_W, label_y,
                              (uint8_t)entries[i].name[j],
                              label_fg, desktop_color);
        }

        icon_idx++;
    }
}

/* ------------------------------------------------------------------ */
/*  Desktop                                                            */
/* ------------------------------------------------------------------ */

void wm_draw_desktop(void) {
    fb_fill_rect(0, 0, fb_info.width, fb_info.height, desktop_color);
    wm_draw_desktop_icons();
}

uint32_t wm_get_desktop_color(void) { return desktop_color; }

/* ------------------------------------------------------------------ */
/*  Window chrome                                                      */
/* ------------------------------------------------------------------ */

void wm_draw_chrome(window_t *win) {
    if (!(win->flags & WIN_FLAG_VISIBLE)) return;

    /* Outer border */
    fb_draw_rect((uint32_t)win->x, (uint32_t)win->y,
                 win->w, win->h, win->border_color);

    /* Title bar background */
    uint32_t tb_x = (uint32_t)win->x + WIN_BORDER_W;
    uint32_t tb_y = (uint32_t)win->y + WIN_BORDER_W;
    uint32_t tb_w = win->w - 2 * WIN_BORDER_W;
    uint32_t tb_h = WIN_TITLEBAR_H - WIN_BORDER_W;

    fb_fill_rect(tb_x, tb_y, tb_w, tb_h, win->title_bg_color);

    /* Title bar bottom separator line */
    fb_draw_hline(tb_x, tb_y + tb_h, tb_w, win->border_color);

    /* Title text (pixel-positioned, 2px top padding, 8px left padding) */
    uint32_t text_px = tb_x + 8;
    uint32_t text_py = tb_y + 2;

    for (int i = 0; win->title[i] != '\0'; i++) {
        if (text_px + FONT_W > tb_x + tb_w) break;
        fb_render_char_px(text_px, text_py, (uint8_t)win->title[i],
                          win->title_fg_color, win->title_bg_color);
        text_px += FONT_W;
    }
}

/* ------------------------------------------------------------------ */
/*  Window creation                                                    */
/* ------------------------------------------------------------------ */

window_t *wm_create_window(int32_t x, int32_t y, uint32_t w, uint32_t h,
                            const char *title) {
    window_t *win = (window_t *)kcalloc(1, sizeof(window_t));
    if (!win) return NULL;

    win->x = x;
    win->y = y;
    win->w = w;
    win->h = h;

    /* Copy title */
    int i;
    for (i = 0; i < WIN_MAX_TITLE - 1 && title[i]; i++)
        win->title[i] = title[i];
    win->title[i] = '\0';

    /* Default colors */
    win->title_bg_color = fb_pack_color(60, 60, 90);
    win->title_fg_color = fb_pack_color(220, 220, 220);
    win->body_bg_color  = fb_pack_color(0, 0, 0);
    win->border_color   = fb_pack_color(100, 102, 110);

    win->flags = WIN_FLAG_VISIBLE | WIN_FLAG_FOCUSED | WIN_FLAG_DRAGGABLE;

    wm_update_content_rect(win);

    if (!shell_win) shell_win = win;

    return win;
}

/* ------------------------------------------------------------------ */
/*  Initialization                                                     */
/* ------------------------------------------------------------------ */

void wm_init(void) {
    desktop_color = fb_pack_color(64, 68, 75);
    desktop_ensure_path();
}

/* ------------------------------------------------------------------ */
/*  Accessors                                                          */
/* ------------------------------------------------------------------ */

window_t *wm_get_shell_window(void) { return shell_win; }

/* ------------------------------------------------------------------ */
/*  Redraw                                                             */
/* ------------------------------------------------------------------ */

void wm_redraw_all(void) {
    mouse_hide_cursor();
    wm_draw_desktop();
    if (shell_win && (shell_win->flags & WIN_FLAG_VISIBLE)) {
        wm_draw_chrome(shell_win);
        fb_console_repaint();
    }
    mouse_show_cursor();
}

void wm_refresh_desktop(void) {
    mouse_hide_cursor();
    wm_draw_desktop();
    if (shell_win && (shell_win->flags & WIN_FLAG_VISIBLE)) {
        wm_draw_chrome(shell_win);
        fb_console_repaint();
    }
    mouse_show_cursor();
}

/* ------------------------------------------------------------------ */
/*  Hit testing                                                        */
/* ------------------------------------------------------------------ */

static int hit_titlebar(window_t *win, int32_t mx, int32_t my) {
    return mx >= win->x && mx < win->x + (int32_t)win->w &&
           my >= win->y && my < win->y + (int32_t)(WIN_TITLEBAR_H + WIN_BORDER_W);
}

/* ------------------------------------------------------------------ */
/*  Drag handling                                                      */
/* ------------------------------------------------------------------ */

static void drag_begin(window_t *win, int32_t mx, int32_t my) {
    win->flags |= WIN_FLAG_DRAGGING;
    win->drag_off_x = mx - win->x;
    win->drag_off_y = my - win->y;
}

static void drag_move(window_t *win, int32_t mx, int32_t my) {
    int32_t new_x = mx - win->drag_off_x;
    int32_t new_y = my - win->drag_off_y;

    /* Clamp so title bar stays partially on screen */
    if (new_x < -(int32_t)(win->w - 40)) new_x = -(int32_t)(win->w - 40);
    if (new_x > (int32_t)fb_info.width - 40) new_x = (int32_t)fb_info.width - 40;
    if (new_y < 0) new_y = 0;
    if (new_y > (int32_t)fb_info.height - (int32_t)WIN_TITLEBAR_H)
        new_y = (int32_t)fb_info.height - (int32_t)WIN_TITLEBAR_H;

    if (new_x == win->x && new_y == win->y) return;

    mouse_hide_cursor();

    /* Redraw full desktop (background + icons) to erase old window area */
    wm_draw_desktop();

    /* Update position */
    win->x = new_x;
    win->y = new_y;
    wm_update_content_rect(win);

    /* Draw chrome at new position */
    wm_draw_chrome(win);

    /* Repaint content from character buffer */
    fb_console_repaint();

    mouse_show_cursor();
}

static void drag_end(window_t *win) {
    win->flags &= ~WIN_FLAG_DRAGGING;
}

/* ------------------------------------------------------------------ */
/*  Event processing                                                   */
/* ------------------------------------------------------------------ */

int wm_process_events(void) {
    event_t e = event_poll();
    if (e.type == EVENT_NONE) return 0;

    /* Left-click: check title bar hit for drag start */
    if (e.type == EVENT_MOUSE_BUTTON && e.mouse_button.pressed &&
        (e.mouse_button.button & MOUSE_BTN_LEFT)) {
        if (shell_win && (shell_win->flags & WIN_FLAG_DRAGGABLE) &&
            hit_titlebar(shell_win, e.mouse_button.x, e.mouse_button.y)) {
            drag_begin(shell_win, e.mouse_button.x, e.mouse_button.y);
            return 1;
        }
    }

    /* Left-release: end drag */
    if (e.type == EVENT_MOUSE_BUTTON && !e.mouse_button.pressed &&
        (e.mouse_button.button & MOUSE_BTN_LEFT)) {
        if (shell_win && (shell_win->flags & WIN_FLAG_DRAGGING)) {
            drag_end(shell_win);
            return 1;
        }
    }

    /* Mouse move during drag */
    if (e.type == EVENT_MOUSE_MOVE) {
        if (shell_win && (shell_win->flags & WIN_FLAG_DRAGGING)) {
            drag_move(shell_win, e.mouse_move.x, e.mouse_move.y);
            return 1;
        }
    }

    return 0;
}
