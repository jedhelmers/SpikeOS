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

/* Window list — doubly linked, bottom to top z-order.
   win_bottom = first painted (back), win_top = last painted (front). */
static window_t *win_bottom = NULL;
static window_t *win_top    = NULL;

/* Desktop icon constants */
#define DESKTOP_PATH "/Users/jedhelmers/Desktop"
#define ICON_W       64
#define ICON_H       68
#define ICON_PAD_X   10
#define ICON_PAD_Y   10
#define ICON_RECT_W  32
#define ICON_RECT_H  32
#define ICON_MAX_LABEL 8
#define ICON_LABEL_ROWS 2

static uint32_t desktop_dir_ino = 0;
static int desktop_dir_valid = 0;

/* Track which window is currently being dragged or resized */
static window_t *dragging_win = NULL;
static window_t *resizing_win = NULL;

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
    /* Create intermediate directories if they don't exist */
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

        /* Draw filename label below rect (up to 2 rows of ICON_MAX_LABEL chars) */
        int total_len = 0;
        while (entries[i].name[total_len]) total_len++;

        int max_chars_per_row = ICON_W / FONT_W;
        if (max_chars_per_row > ICON_MAX_LABEL) max_chars_per_row = ICON_MAX_LABEL;

        int pos = 0;
        for (int lr = 0; lr < ICON_LABEL_ROWS && pos < total_len; lr++) {
            int row_len = total_len - pos;
            if (row_len > max_chars_per_row) row_len = max_chars_per_row;

            uint32_t label_y = ry + ICON_RECT_H + 2 + (uint32_t)lr * FONT_H;
            uint32_t label_w = (uint32_t)row_len * FONT_W;
            uint32_t label_x = cx + (ICON_W - label_w) / 2;

            for (int j = 0; j < row_len; j++) {
                fb_render_char_px(label_x + (uint32_t)j * FONT_W, label_y,
                                  (uint8_t)entries[i].name[pos + j],
                                  label_fg, desktop_color);
            }
            pos += row_len;
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
/*  Anti-aliased rounded corner                                        */
/* ------------------------------------------------------------------ */

/*
 * Draw one quarter-circle corner with 2×2 sub-pixel anti-aliasing.
 * Handles interior fill, 1px border, and exterior mask in a single pass.
 * (ox, oy) = top-left of the r×r pixel block.
 * flip_x/flip_y select quadrant: (0,0)=TL, (1,0)=TR, (0,1)=BL, (1,1)=BR
 */
static void draw_aa_corner(uint32_t ox, uint32_t oy, uint32_t r,
                           uint32_t fill_color, uint32_t border_color,
                           uint32_t outside_color, int flip_x, int flip_y) {
    if (r == 0) return;
    int ri = (int)r;

    /* Thresholds in 4× fixed-point (each pixel spans 4 sub-units) */
    int outer_r2 = 16 * ri * ri;
    int inner_r2 = 16 * (ri - 1) * (ri - 1);

    /* Pre-extract colour channels for blending */
    uint32_t rmask = (1u << fb_info.red_mask) - 1;
    uint32_t gmask = (1u << fb_info.green_mask) - 1;
    uint32_t bmask = (1u << fb_info.blue_mask) - 1;

    uint32_t fr = (fill_color    >> fb_info.red_pos)   & rmask;
    uint32_t fg = (fill_color    >> fb_info.green_pos) & gmask;
    uint32_t fb_ = (fill_color   >> fb_info.blue_pos)  & bmask;
    uint32_t bdr = (border_color >> fb_info.red_pos)   & rmask;
    uint32_t bdg = (border_color >> fb_info.green_pos) & gmask;
    uint32_t bdb = (border_color >> fb_info.blue_pos)  & bmask;
    uint32_t otr = (outside_color >> fb_info.red_pos)  & rmask;
    uint32_t otg = (outside_color >> fb_info.green_pos)& gmask;
    uint32_t otb = (outside_color >> fb_info.blue_pos) & bmask;

    for (int j = 0; j < ri; j++) {
        for (int i = 0; i < ri; i++) {
            int nf = 0, nb = 0, no = 0;

            /* 2×2 sub-pixel samples at quarter-pixel offsets */
            static const int sp[4][2] = {{1,1},{3,1},{1,3},{3,3}};
            for (int s = 0; s < 4; s++) {
                int sx = 4 * i + sp[s][0];
                int sy = 4 * j + sp[s][1];
                int dx = flip_x ? sx : (4 * ri - sx);
                int dy = flip_y ? sy : (4 * ri - sy);
                int d2 = dx * dx + dy * dy;

                if (d2 > outer_r2)      no++;
                else if (d2 > inner_r2) nb++;
                else                    nf++;
            }

            uint32_t color;
            if (nf == 4)      color = fill_color;
            else if (no == 4) color = outside_color;
            else if (nb == 4) color = border_color;
            else {
                uint32_t rr = (fr * nf + bdr * nb + otr * no + 2) >> 2;
                uint32_t gg = (fg * nf + bdg * nb + otg * no + 2) >> 2;
                uint32_t bb = (fb_ * nf + bdb * nb + otb * no + 2) >> 2;
                color = fb_pack_color((uint8_t)rr, (uint8_t)gg, (uint8_t)bb);
            }

            fb_putpixel(ox + (uint32_t)i, oy + (uint32_t)j, color);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Window chrome                                                      */
/* ------------------------------------------------------------------ */

void wm_draw_chrome(window_t *win) {
    if (!(win->flags & WIN_FLAG_VISIBLE)) return;

    uint32_t wx = (uint32_t)win->x;
    uint32_t wy = (uint32_t)win->y;
    uint32_t ww = win->w;
    uint32_t wh = win->h;
    uint32_t r  = WIN_BORDER_RADIUS;

    /* --- Fill body background (rectangular, corners will be masked) --- */
    fb_fill_rect(wx, wy, ww, wh, win->body_bg_color);

    /* --- Title bar background --- */
    uint32_t tb_x = wx + WIN_BORDER_W;
    uint32_t tb_y = wy + WIN_BORDER_W;
    uint32_t tb_w = ww - 2 * WIN_BORDER_W;
    uint32_t tb_h = WIN_TITLEBAR_H - WIN_BORDER_W;
    fb_fill_rect(tb_x, tb_y, tb_w, tb_h, win->title_bg_color);

    /* --- Anti-aliased rounded corners (fill + border + mask in one pass) --- */
    draw_aa_corner(wx, wy, r,
                   win->title_bg_color, win->border_color, desktop_color, 0, 0);
    draw_aa_corner(wx + ww - r, wy, r,
                   win->title_bg_color, win->border_color, desktop_color, 1, 0);
    draw_aa_corner(wx, wy + wh - r, r,
                   win->body_bg_color, win->border_color, desktop_color, 0, 1);
    draw_aa_corner(wx + ww - r, wy + wh - r, r,
                   win->body_bg_color, win->border_color, desktop_color, 1, 1);

    /* --- Straight border edges (between rounded corners) --- */
    fb_draw_hline(wx + r, wy, ww - 2 * r, win->border_color);           /* top */
    fb_draw_hline(wx + r, wy + wh - 1, ww - 2 * r, win->border_color);  /* bottom */
    fb_draw_vline(wx, wy + r, wh - 2 * r, win->border_color);           /* left */
    fb_draw_vline(wx + ww - 1, wy + r, wh - 2 * r, win->border_color);  /* right */

    /* --- Title bar separator line --- */
    fb_draw_hline(tb_x, tb_y + tb_h, tb_w, win->border_color);

    /* --- Traffic light dots --- */
    uint32_t dot_cy = wy + WIN_DOT_Y_OFF;
    fb_fill_circle(wx + WIN_DOT_CLOSE_X, dot_cy, WIN_DOT_RADIUS,
                   fb_pack_color(255, 95, 87));    /* close — red */
    fb_fill_circle(wx + WIN_DOT_MIN_X, dot_cy, WIN_DOT_RADIUS,
                   fb_pack_color(255, 189, 46));   /* minimize — yellow */
    fb_fill_circle(wx + WIN_DOT_MAX_X, dot_cy, WIN_DOT_RADIUS,
                   fb_pack_color(39, 201, 63));    /* maximize — green */

    /* --- Title text (shifted right past the dots) --- */
    uint32_t text_px = tb_x + 60;
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

    win->flags = WIN_FLAG_VISIBLE | WIN_FLAG_FOCUSED | WIN_FLAG_DRAGGABLE | WIN_FLAG_RESIZABLE;
    win->next = NULL;
    win->prev = NULL;

    wm_update_content_rect(win);

    /* Insert at top of z-order */
    if (!win_bottom) {
        win_bottom = win_top = win;
    } else {
        win->prev = win_top;
        win_top->next = win;
        win_top = win;
    }

    if (!shell_win) shell_win = win;

    return win;
}

/* ------------------------------------------------------------------ */
/*  Window destruction                                                 */
/* ------------------------------------------------------------------ */

void wm_destroy_window(window_t *win) {
    if (!win) return;

    /* Unlink from doubly-linked z-order list */
    if (win->prev) win->prev->next = win->next;
    else           win_bottom = win->next;
    if (win->next) win->next->prev = win->prev;
    else           win_top = win->prev;

    /* Clear shell_win if this was it (shouldn't happen, but safety) */
    if (win == shell_win) shell_win = NULL;

    /* Clear drag/resize tracking if this window was active */
    if (win == dragging_win)  dragging_win = NULL;
    if (win == resizing_win)  resizing_win = NULL;

    kfree(win);

    /* Focus the new top window */
    if (win_top) {
        for (window_t *w = win_bottom; w; w = w->next)
            w->flags &= ~WIN_FLAG_FOCUSED;
        win_top->flags |= WIN_FLAG_FOCUSED;
    }

    wm_redraw_all();
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
window_t *wm_get_top_window(void)   { return win_top; }

/* ------------------------------------------------------------------ */
/*  Redraw                                                             */
/* ------------------------------------------------------------------ */

void wm_redraw_all(void) {
    mouse_hide_cursor();
    wm_draw_desktop();

    /* Paint all visible windows bottom-to-top */
    for (window_t *w = win_bottom; w; w = w->next) {
        if (w->flags & WIN_FLAG_VISIBLE) {
            wm_draw_chrome(w);
            /* Repaint content for the shell window (others would need their
               own repaint callbacks — for now, only shell has one) */
            if (w == shell_win)
                fb_console_repaint();
        }
    }

    mouse_show_cursor();
}

void wm_refresh_desktop(void) {
    wm_redraw_all();
}

/* ------------------------------------------------------------------ */
/*  Hit testing                                                        */
/* ------------------------------------------------------------------ */

static int hit_titlebar(window_t *win, int32_t mx, int32_t my) {
    return mx >= win->x && mx < win->x + (int32_t)win->w &&
           my >= win->y && my < win->y + (int32_t)(WIN_TITLEBAR_H + WIN_BORDER_W);
}

static int hit_window(window_t *win, int32_t mx, int32_t my) {
    return mx >= win->x && mx < win->x + (int32_t)win->w &&
           my >= win->y && my < win->y + (int32_t)win->h;
}

/* Detect which resize edges the mouse is near. Returns 0 if not on any edge. */
static uint32_t hit_resize_edges(window_t *win, int32_t mx, int32_t my) {
    if (!(win->flags & WIN_FLAG_RESIZABLE)) return 0;
    if (!hit_window(win, mx, my)) return 0;

    uint32_t edges = 0;
    int32_t grip = WIN_RESIZE_GRIP;

    if (mx < win->x + grip)                          edges |= RESIZE_LEFT;
    if (mx >= win->x + (int32_t)win->w - grip)       edges |= RESIZE_RIGHT;
    if (my < win->y + grip)                           edges |= RESIZE_TOP;
    if (my >= win->y + (int32_t)win->h - grip)        edges |= RESIZE_BOTTOM;

    return edges;
}

/* Find the topmost window at (mx, my), searching top-to-bottom */
window_t *wm_window_at(int32_t mx, int32_t my) {
    for (window_t *w = win_top; w; w = w->prev) {
        if ((w->flags & WIN_FLAG_VISIBLE) && hit_window(w, mx, my))
            return w;
    }
    return NULL;
}

/* Bring a window to the front of the z-order */
void wm_focus_window(window_t *win) {
    if (!win || win == win_top) return;

    /* Remove from current position */
    if (win->prev) win->prev->next = win->next;
    else           win_bottom = win->next;
    if (win->next) win->next->prev = win->prev;
    else           win_top = win->prev;

    /* Insert at top */
    win->prev = win_top;
    win->next = NULL;
    if (win_top) win_top->next = win;
    win_top = win;
    if (!win_bottom) win_bottom = win;

    /* Update focus flags */
    for (window_t *w = win_bottom; w; w = w->next)
        w->flags &= ~WIN_FLAG_FOCUSED;
    win->flags |= WIN_FLAG_FOCUSED;
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

    /* Erase old window area with desktop color (dirty-rect) instead of
       repainting the entire screen.  Clamp to visible screen bounds. */
    {
        int32_t ox = win->x;
        int32_t oy = win->y;
        int32_t ow = (int32_t)win->w;
        int32_t oh = (int32_t)win->h;

        /* Clamp to screen */
        if (ox < 0) { ow += ox; ox = 0; }
        if (oy < 0) { oh += oy; oy = 0; }
        if (ox + ow > (int32_t)fb_info.width)  ow = (int32_t)fb_info.width - ox;
        if (oy + oh > (int32_t)fb_info.height) oh = (int32_t)fb_info.height - oy;

        if (ow > 0 && oh > 0) {
            fb_fill_rect((uint32_t)ox, (uint32_t)oy,
                         (uint32_t)ow, (uint32_t)oh, desktop_color);
        }
    }

    /* Redraw desktop icons (lightweight — only icons, not the full background) */
    wm_draw_desktop_icons();

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
/*  Resize handling                                                    */
/* ------------------------------------------------------------------ */

static void resize_begin(window_t *win, int32_t mx, int32_t my, uint32_t edges) {
    win->flags |= WIN_FLAG_RESIZING;
    win->resize_edges = edges;
    win->resize_anchor_x = mx;
    win->resize_anchor_y = my;
    win->resize_orig_x = win->x;
    win->resize_orig_y = win->y;
    win->resize_orig_w = win->w;
    win->resize_orig_h = win->h;
}

static void resize_move(window_t *win, int32_t mx, int32_t my) {
    int32_t dx = mx - win->resize_anchor_x;
    int32_t dy = my - win->resize_anchor_y;

    int32_t new_x = win->resize_orig_x;
    int32_t new_y = win->resize_orig_y;
    int32_t new_w = (int32_t)win->resize_orig_w;
    int32_t new_h = (int32_t)win->resize_orig_h;

    if (win->resize_edges & RESIZE_RIGHT)  new_w += dx;
    if (win->resize_edges & RESIZE_BOTTOM) new_h += dy;
    if (win->resize_edges & RESIZE_LEFT) {
        new_x += dx;
        new_w -= dx;
    }
    if (win->resize_edges & RESIZE_TOP) {
        new_y += dy;
        new_h -= dy;
    }

    /* Enforce minimum size */
    if (new_w < WIN_MIN_W) {
        if (win->resize_edges & RESIZE_LEFT)
            new_x = win->resize_orig_x + (int32_t)win->resize_orig_w - WIN_MIN_W;
        new_w = WIN_MIN_W;
    }
    if (new_h < WIN_MIN_H) {
        if (win->resize_edges & RESIZE_TOP)
            new_y = win->resize_orig_y + (int32_t)win->resize_orig_h - WIN_MIN_H;
        new_h = WIN_MIN_H;
    }

    if (new_x == win->x && new_y == win->y &&
        new_w == (int32_t)win->w && new_h == (int32_t)win->h)
        return;

    mouse_hide_cursor();

    /* Erase old window area */
    {
        int32_t ox = win->x, oy = win->y;
        int32_t ow = (int32_t)win->w, oh = (int32_t)win->h;
        if (ox < 0) { ow += ox; ox = 0; }
        if (oy < 0) { oh += oy; oy = 0; }
        if (ox + ow > (int32_t)fb_info.width)  ow = (int32_t)fb_info.width - ox;
        if (oy + oh > (int32_t)fb_info.height) oh = (int32_t)fb_info.height - oy;
        if (ow > 0 && oh > 0)
            fb_fill_rect((uint32_t)ox, (uint32_t)oy,
                         (uint32_t)ow, (uint32_t)oh, desktop_color);
    }

    wm_draw_desktop_icons();

    /* Apply new geometry */
    win->x = new_x;
    win->y = new_y;
    win->w = (uint32_t)new_w;
    win->h = (uint32_t)new_h;
    wm_update_content_rect(win);

    /* Rebind console to new size if this is the shell window */
    if (win == shell_win)
        fb_console_bind_window(win);

    wm_draw_chrome(win);
    fb_console_repaint();

    mouse_show_cursor();
}

static void resize_end(window_t *win) {
    win->flags &= ~WIN_FLAG_RESIZING;
    win->resize_edges = 0;
}

/* ------------------------------------------------------------------ */
/*  Event processing                                                   */
/* ------------------------------------------------------------------ */

int wm_process_events(void) {
    event_t e = event_poll();
    if (e.type == EVENT_NONE) return 0;

    /* Left-click: find the topmost window under the mouse */
    if (e.type == EVENT_MOUSE_BUTTON && e.mouse_button.pressed &&
        (e.mouse_button.button & MOUSE_BTN_LEFT)) {

        window_t *hit = wm_window_at(e.mouse_button.x, e.mouse_button.y);
        if (hit) {
            /* Click-to-focus: bring to front if not already on top */
            if (hit != win_top) {
                wm_focus_window(hit);
                wm_redraw_all();
            }

            /* Check for resize grip first (edges/corners) */
            uint32_t edges = hit_resize_edges(hit,
                                e.mouse_button.x, e.mouse_button.y);
            if (edges) {
                resize_begin(hit, e.mouse_button.x, e.mouse_button.y, edges);
                resizing_win = hit;
                return 1;
            }

            /* Check traffic light dots before starting drag */
            if (hit_titlebar(hit, e.mouse_button.x, e.mouse_button.y)) {
                int32_t rel_x = e.mouse_button.x - hit->x;
                int32_t rel_y = e.mouse_button.y - hit->y;

                /* Close dot — set flag; owner checks and cleans up */
                if ((rel_x - WIN_DOT_CLOSE_X) * (rel_x - WIN_DOT_CLOSE_X) +
                    (rel_y - WIN_DOT_Y_OFF) * (rel_y - WIN_DOT_Y_OFF) <=
                    WIN_DOT_RADIUS * WIN_DOT_RADIUS) {
                    if (hit != shell_win)
                        hit->flags |= WIN_FLAG_CLOSE_REQ;
                    return 1;
                }

                /* Minimize dot */
                if ((rel_x - WIN_DOT_MIN_X) * (rel_x - WIN_DOT_MIN_X) +
                    (rel_y - WIN_DOT_Y_OFF) * (rel_y - WIN_DOT_Y_OFF) <=
                    WIN_DOT_RADIUS * WIN_DOT_RADIUS) {
                    hit->flags &= ~WIN_FLAG_VISIBLE;
                    wm_redraw_all();
                    return 1;
                }

                /* Maximize dot */
                if ((rel_x - WIN_DOT_MAX_X) * (rel_x - WIN_DOT_MAX_X) +
                    (rel_y - WIN_DOT_Y_OFF) * (rel_y - WIN_DOT_Y_OFF) <=
                    WIN_DOT_RADIUS * WIN_DOT_RADIUS) {
                    if (hit->flags & WIN_FLAG_MAXIMIZED) {
                        hit->x = hit->saved_x;
                        hit->y = hit->saved_y;
                        hit->w = hit->saved_w;
                        hit->h = hit->saved_h;
                        hit->flags &= ~WIN_FLAG_MAXIMIZED;
                    } else {
                        hit->saved_x = hit->x;
                        hit->saved_y = hit->y;
                        hit->saved_w = hit->w;
                        hit->saved_h = hit->h;
                        hit->x = 0;
                        hit->y = 0;
                        hit->w = fb_info.width;
                        hit->h = fb_info.height;
                        hit->flags |= WIN_FLAG_MAXIMIZED;
                    }
                    wm_update_content_rect(hit);
                    if (hit == shell_win)
                        fb_console_bind_window(hit);
                    wm_redraw_all();
                    return 1;
                }

                /* No dot hit — start drag */
                if (hit->flags & WIN_FLAG_DRAGGABLE) {
                    drag_begin(hit, e.mouse_button.x, e.mouse_button.y);
                    dragging_win = hit;
                }
            }
            return 1;
        }
    }

    /* Left-release: end drag or resize */
    if (e.type == EVENT_MOUSE_BUTTON && !e.mouse_button.pressed &&
        (e.mouse_button.button & MOUSE_BTN_LEFT)) {
        if (resizing_win && (resizing_win->flags & WIN_FLAG_RESIZING)) {
            resize_end(resizing_win);
            resizing_win = NULL;
            return 1;
        }
        if (dragging_win && (dragging_win->flags & WIN_FLAG_DRAGGING)) {
            drag_end(dragging_win);
            dragging_win = NULL;
            return 1;
        }
    }

    /* Mouse move during drag or resize */
    if (e.type == EVENT_MOUSE_MOVE) {
        if (resizing_win && (resizing_win->flags & WIN_FLAG_RESIZING)) {
            resize_move(resizing_win, e.mouse_move.x, e.mouse_move.y);
            return 1;
        }
        if (dragging_win && (dragging_win->flags & WIN_FLAG_DRAGGING)) {
            drag_move(dragging_win, e.mouse_move.x, e.mouse_move.y);
            return 1;
        }
    }

    return 0;
}
