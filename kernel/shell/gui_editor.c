#include <kernel/gui_editor.h>
#include <kernel/window.h>
#include <kernel/keyboard.h>
#include <kernel/key_event.h>
#include <kernel/framebuffer.h>
#include <kernel/fb_console.h>
#include <kernel/mouse.h>
#include <kernel/vfs.h>
#include <kernel/heap.h>
#include <kernel/process.h>
#include <kernel/event.h>
#include <kernel/hal.h>
#include <string.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/*  Constants                                                          */
/* ------------------------------------------------------------------ */

#define FONT_W 8
#define FONT_H 16
#define GE_MAX_LINES      1024
#define GE_INIT_LINE_CAP   128
#define GE_STATUS_MAX       80
#define MAX_GUI_EDITORS      4

/* Colors */
#define GE_FG     fb_pack_color(220, 220, 220)
#define GE_BG     fb_pack_color(0, 0, 0)
#define GE_BAR_FG fb_pack_color(0, 0, 0)
#define GE_BAR_BG fb_pack_color(200, 200, 200)
#define GE_CURSOR fb_pack_color(220, 220, 220)

/* ------------------------------------------------------------------ */
/*  Editor instance state                                              */
/* ------------------------------------------------------------------ */

typedef struct gui_editor {
    int active;
    window_t *win;
    char filename[128];

    char *lines[GE_MAX_LINES];
    int line_len[GE_MAX_LINES];
    int line_cap[GE_MAX_LINES];
    int nlines;

    int cx, cy;        /* cursor col, row in file */
    int scroll;        /* first visible line */
    int text_rows;     /* rows available for text */
    int text_cols;     /* cols available for text */
    int modified;
    int quit;
    char status[GE_STATUS_MAX];
} gui_editor_t;

static gui_editor_t editors[MAX_GUI_EDITORS];
static int pending_slot = -1;

/* ------------------------------------------------------------------ */
/*  Line buffer management                                             */
/* ------------------------------------------------------------------ */

static char *ge_line_alloc(int cap) {
    char *p = (char *)kmalloc((size_t)cap);
    if (p) p[0] = '\0';
    return p;
}

static void ge_line_ensure(gui_editor_t *ed, int idx, int needed) {
    if (needed < ed->line_cap[idx]) return;
    int new_cap = ed->line_cap[idx];
    while (new_cap <= needed) new_cap *= 2;
    char *p = (char *)krealloc(ed->lines[idx], (size_t)new_cap);
    if (p) {
        ed->lines[idx] = p;
        ed->line_cap[idx] = new_cap;
    }
}

static void ge_free_lines(gui_editor_t *ed) {
    for (int i = 0; i < ed->nlines; i++) {
        if (ed->lines[i]) kfree(ed->lines[i]);
        ed->lines[i] = NULL;
    }
    ed->nlines = 0;
}

static int ge_insert_line(gui_editor_t *ed, int idx) {
    if (ed->nlines >= GE_MAX_LINES) return -1;
    for (int i = ed->nlines; i > idx; i--) {
        ed->lines[i] = ed->lines[i - 1];
        ed->line_len[i] = ed->line_len[i - 1];
        ed->line_cap[i] = ed->line_cap[i - 1];
    }
    ed->lines[idx] = ge_line_alloc(GE_INIT_LINE_CAP);
    ed->line_len[idx] = 0;
    ed->line_cap[idx] = GE_INIT_LINE_CAP;
    ed->nlines++;
    return 0;
}

static void ge_delete_line(gui_editor_t *ed, int idx) {
    if (idx < 0 || idx >= ed->nlines) return;
    kfree(ed->lines[idx]);
    for (int i = idx; i < ed->nlines - 1; i++) {
        ed->lines[i] = ed->lines[i + 1];
        ed->line_len[i] = ed->line_len[i + 1];
        ed->line_cap[i] = ed->line_cap[i + 1];
    }
    ed->nlines--;
    if (ed->nlines == 0) {
        ed->lines[0] = ge_line_alloc(GE_INIT_LINE_CAP);
        ed->line_len[0] = 0;
        ed->line_cap[0] = GE_INIT_LINE_CAP;
        ed->nlines = 1;
    }
}

/* ------------------------------------------------------------------ */
/*  File I/O                                                           */
/* ------------------------------------------------------------------ */

static void ge_load_file(gui_editor_t *ed) {
    ed->nlines = 0;

    int32_t ino = vfs_resolve(ed->filename, NULL, NULL);
    if (ino < 0) {
        ed->lines[0] = ge_line_alloc(GE_INIT_LINE_CAP);
        ed->line_len[0] = 0;
        ed->line_cap[0] = GE_INIT_LINE_CAP;
        ed->nlines = 1;
        return;
    }

    vfs_inode_t *node = vfs_get_inode((uint32_t)ino);
    if (!node || node->type != VFS_TYPE_FILE || node->size == 0) {
        ed->lines[0] = ge_line_alloc(GE_INIT_LINE_CAP);
        ed->line_len[0] = 0;
        ed->line_cap[0] = GE_INIT_LINE_CAP;
        ed->nlines = 1;
        return;
    }

    const char *data = (const char *)node->data;
    uint32_t size = node->size;
    uint32_t start = 0;

    for (uint32_t i = 0; i <= size && ed->nlines < GE_MAX_LINES; i++) {
        if (i == size || data[i] == '\n') {
            int len = (int)(i - start);
            int cap = GE_INIT_LINE_CAP;
            while (cap <= len) cap *= 2;
            ed->lines[ed->nlines] = ge_line_alloc(cap);
            if (ed->lines[ed->nlines] && len > 0)
                memcpy(ed->lines[ed->nlines], data + start, (size_t)len);
            if (ed->lines[ed->nlines])
                ed->lines[ed->nlines][len] = '\0';
            ed->line_len[ed->nlines] = len;
            ed->line_cap[ed->nlines] = cap;
            ed->nlines++;
            start = i + 1;
        }
    }

    if (ed->nlines == 0) {
        ed->lines[0] = ge_line_alloc(GE_INIT_LINE_CAP);
        ed->line_len[0] = 0;
        ed->line_cap[0] = GE_INIT_LINE_CAP;
        ed->nlines = 1;
    }
}

static int ge_save_file(gui_editor_t *ed) {
    int32_t ino = vfs_resolve(ed->filename, NULL, NULL);
    if (ino < 0) {
        ino = vfs_create_file(ed->filename);
        if (ino < 0) return -1;
    }

    vfs_inode_t *node = vfs_get_inode((uint32_t)ino);
    if (!node || node->type != VFS_TYPE_FILE) return -1;

    uint32_t total = 0;
    for (int i = 0; i < ed->nlines; i++) {
        total += (uint32_t)ed->line_len[i];
        if (i < ed->nlines - 1) total++;
    }

    uint32_t off = 0;
    for (int i = 0; i < ed->nlines; i++) {
        if (ed->line_len[i] > 0)
            vfs_write((uint32_t)ino, ed->lines[i], off, (uint32_t)ed->line_len[i]);
        off += (uint32_t)ed->line_len[i];
        if (i < ed->nlines - 1) {
            vfs_write((uint32_t)ino, "\n", off, 1);
            off++;
        }
    }

    node->size = total;
    ed->modified = 0;
    strcpy(ed->status, "Saved");
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Rendering                                                          */
/* ------------------------------------------------------------------ */

static void ge_compute_dims(gui_editor_t *ed) {
    if (!ed->win) return;
    ed->text_cols = (int)(ed->win->content_w / FONT_W);
    ed->text_rows = (int)(ed->win->content_h / FONT_H) - 1;  /* -1 for status bar */
    if (ed->text_rows < 1) ed->text_rows = 1;
}

static void ge_putchar_at(gui_editor_t *ed, int x, int y, char ch,
                           uint32_t fg, uint32_t bg) {
    if (!ed->win) return;
    if (x < 0 || y < 0 || x >= ed->text_cols || y >= (int)(ed->win->content_h / FONT_H))
        return;
    uint32_t px = ed->win->content_x + (uint32_t)x * FONT_W;
    uint32_t py = ed->win->content_y + (uint32_t)y * FONT_H;
    fb_render_char_px(px, py, (uint8_t)ch, fg, bg);
}

static void ge_fill_row(gui_editor_t *ed, int y, uint32_t fg, uint32_t bg) {
    for (int x = 0; x < ed->text_cols; x++)
        ge_putchar_at(ed, x, y, ' ', fg, bg);
}

static void ge_draw_str(gui_editor_t *ed, int x, int y, const char *s,
                         uint32_t fg, uint32_t bg) {
    for (int i = 0; s[i] && x + i < ed->text_cols; i++)
        ge_putchar_at(ed, x + i, y, s[i], fg, bg);
}

static void gui_editor_draw(gui_editor_t *ed) {
    if (!ed->win) return;

    ge_compute_dims(ed);
    uint32_t fg = GE_FG;
    uint32_t bg = GE_BG;

    /* Clear content area */
    fb_fill_rect(ed->win->content_x, ed->win->content_y,
                 ed->win->content_w, ed->win->content_h, bg);

    /* Draw text lines */
    for (int row = 0; row < ed->text_rows; row++) {
        int file_line = ed->scroll + row;
        if (file_line >= ed->nlines) {
            ge_putchar_at(ed, 0, row, '~', fb_pack_color(80, 80, 80), bg);
        } else {
            int len = ed->line_len[file_line];
            for (int col = 0; col < ed->text_cols; col++) {
                if (col < len)
                    ge_putchar_at(ed, col, row, ed->lines[file_line][col], fg, bg);
            }
        }
    }

    /* Draw status bar at bottom */
    int status_y = (int)(ed->win->content_h / FONT_H) - 1;
    ge_fill_row(ed, status_y, GE_BAR_FG, GE_BAR_BG);

    /* Left: status or filename */
    if (ed->status[0])
        ge_draw_str(ed, 1, status_y, ed->status, GE_BAR_FG, GE_BAR_BG);
    else {
        /* Show filename and modified indicator */
        ge_draw_str(ed, 1, status_y, ed->filename, GE_BAR_FG, GE_BAR_BG);
        if (ed->modified) {
            int flen = 0;
            while (ed->filename[flen]) flen++;
            ge_draw_str(ed, 1 + flen + 1, status_y, "[Modified]",
                        GE_BAR_FG, GE_BAR_BG);
        }
    }

    /* Right: line/col */
    char pos[32];
    int pi = 0;
    const char *lbl = "Ln ";
    for (int i = 0; lbl[i]; i++) pos[pi++] = lbl[i];

    int tmp = ed->cy + 1;
    char num[12];
    int ni = 0;
    if (tmp == 0) num[ni++] = '0';
    else { while (tmp > 0) { num[ni++] = '0' + (tmp % 10); tmp /= 10; } }
    for (int i = ni - 1; i >= 0; i--) pos[pi++] = num[i];

    pos[pi++] = ',';
    pos[pi++] = ' ';
    lbl = "Col ";
    for (int i = 0; lbl[i]; i++) pos[pi++] = lbl[i];

    tmp = ed->cx + 1;
    ni = 0;
    if (tmp == 0) num[ni++] = '0';
    else { while (tmp > 0) { num[ni++] = '0' + (tmp % 10); tmp /= 10; } }
    for (int i = ni - 1; i >= 0; i--) pos[pi++] = num[i];
    pos[pi] = '\0';

    int pos_x = ed->text_cols - pi - 1;
    if (pos_x > 0)
        ge_draw_str(ed, pos_x, status_y, pos, GE_BAR_FG, GE_BAR_BG);

    /* Draw cursor (underline) */
    int cur_screen_y = ed->cy - ed->scroll;
    if (cur_screen_y >= 0 && cur_screen_y < ed->text_rows) {
        uint32_t cpx = ed->win->content_x + (uint32_t)ed->cx * FONT_W;
        uint32_t cpy = ed->win->content_y + (uint32_t)cur_screen_y * FONT_H + 14;
        fb_fill_rect(cpx, cpy, FONT_W, 2, GE_CURSOR);
    }
}

/* Repaint callback for the window manager */
static void ge_repaint_cb(window_t *win) {
    for (int i = 0; i < MAX_GUI_EDITORS; i++) {
        if (editors[i].active && editors[i].win == win) {
            gui_editor_draw(&editors[i]);
            return;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Menu action callbacks                                              */
/* ------------------------------------------------------------------ */

static void ge_action_save(void *ctx) {
    gui_editor_t *ed = (gui_editor_t *)ctx;
    ge_save_file(ed);
}

static void ge_action_quit(void *ctx) {
    gui_editor_t *ed = (gui_editor_t *)ctx;
    ed->quit = 1;
}

/* ------------------------------------------------------------------ */
/*  Cursor helpers                                                     */
/* ------------------------------------------------------------------ */

static void ge_clamp_cx(gui_editor_t *ed) {
    if (ed->cx > ed->line_len[ed->cy])
        ed->cx = ed->line_len[ed->cy];
}

static void ge_scroll_to_cursor(gui_editor_t *ed) {
    ge_compute_dims(ed);
    if (ed->cy < ed->scroll)
        ed->scroll = ed->cy;
    if (ed->cy >= ed->scroll + ed->text_rows)
        ed->scroll = ed->cy - ed->text_rows + 1;
}

/* ------------------------------------------------------------------ */
/*  Text editing                                                       */
/* ------------------------------------------------------------------ */

static void ge_insert_char(gui_editor_t *ed, char c) {
    ge_line_ensure(ed, ed->cy, ed->line_len[ed->cy] + 1);
    for (int i = ed->line_len[ed->cy]; i > ed->cx; i--)
        ed->lines[ed->cy][i] = ed->lines[ed->cy][i - 1];
    ed->lines[ed->cy][ed->cx] = c;
    ed->line_len[ed->cy]++;
    ed->lines[ed->cy][ed->line_len[ed->cy]] = '\0';
    ed->cx++;
    ed->modified = 1;
}

static void ge_insert_newline(gui_editor_t *ed) {
    if (ed->nlines >= GE_MAX_LINES) return;
    int tail_len = ed->line_len[ed->cy] - ed->cx;
    ge_insert_line(ed, ed->cy + 1);
    if (tail_len > 0) {
        ge_line_ensure(ed, ed->cy + 1, tail_len);
        memcpy(ed->lines[ed->cy + 1], ed->lines[ed->cy] + ed->cx, (size_t)tail_len);
        ed->lines[ed->cy + 1][tail_len] = '\0';
        ed->line_len[ed->cy + 1] = tail_len;
    }
    ed->line_len[ed->cy] = ed->cx;
    ed->lines[ed->cy][ed->cx] = '\0';
    ed->cy++;
    ed->cx = 0;
    ed->modified = 1;
}

static void ge_backspace(gui_editor_t *ed) {
    if (ed->cx > 0) {
        for (int i = ed->cx - 1; i < ed->line_len[ed->cy] - 1; i++)
            ed->lines[ed->cy][i] = ed->lines[ed->cy][i + 1];
        ed->line_len[ed->cy]--;
        ed->lines[ed->cy][ed->line_len[ed->cy]] = '\0';
        ed->cx--;
        ed->modified = 1;
    } else if (ed->cy > 0) {
        int prev = ed->cy - 1;
        int old_len = ed->line_len[prev];
        int cur_len = ed->line_len[ed->cy];
        ge_line_ensure(ed, prev, old_len + cur_len);
        memcpy(ed->lines[prev] + old_len, ed->lines[ed->cy], (size_t)cur_len);
        ed->line_len[prev] = old_len + cur_len;
        ed->lines[prev][ed->line_len[prev]] = '\0';
        ge_delete_line(ed, ed->cy);
        ed->cy = prev;
        ed->cx = old_len;
        ed->modified = 1;
    }
}

static void ge_delete(gui_editor_t *ed) {
    if (ed->cx < ed->line_len[ed->cy]) {
        for (int i = ed->cx; i < ed->line_len[ed->cy] - 1; i++)
            ed->lines[ed->cy][i] = ed->lines[ed->cy][i + 1];
        ed->line_len[ed->cy]--;
        ed->lines[ed->cy][ed->line_len[ed->cy]] = '\0';
        ed->modified = 1;
    } else if (ed->cy < ed->nlines - 1) {
        int next = ed->cy + 1;
        int cur_len = ed->line_len[ed->cy];
        int next_len = ed->line_len[next];
        ge_line_ensure(ed, ed->cy, cur_len + next_len);
        memcpy(ed->lines[ed->cy] + cur_len, ed->lines[next], (size_t)next_len);
        ed->line_len[ed->cy] = cur_len + next_len;
        ed->lines[ed->cy][ed->line_len[ed->cy]] = '\0';
        ge_delete_line(ed, next);
        ed->modified = 1;
    }
}

static void ge_cut_line(gui_editor_t *ed) {
    ge_delete_line(ed, ed->cy);
    if (ed->cy >= ed->nlines)
        ed->cy = ed->nlines - 1;
    ge_clamp_cx(ed);
    ed->modified = 1;
}

/* ------------------------------------------------------------------ */
/*  Thread entry point                                                 */
/* ------------------------------------------------------------------ */

static void gui_editor_thread(void) {
    /* Claim our slot */
    int slot = pending_slot;
    pending_slot = -1;
    if (slot < 0 || slot >= MAX_GUI_EDITORS) return;

    gui_editor_t *ed = &editors[slot];

    /* Create window — extract just the filename for the title */
    const char *base = ed->filename;
    for (const char *p = ed->filename; *p; p++) {
        if (*p == '/') base = p + 1;
    }
    char title[WIN_MAX_TITLE];
    int ti = 0;
    const char *pfx = "Edit: ";
    for (int i = 0; pfx[i] && ti < WIN_MAX_TITLE - 1; i++)
        title[ti++] = pfx[i];
    for (int i = 0; base[i] && ti < WIN_MAX_TITLE - 1; i++)
        title[ti++] = base[i];
    title[ti] = '\0';

    /* Create window centered */
    uint32_t win_w = 640;
    uint32_t win_h = 480;
    if (win_w > fb_info.width - 20) win_w = fb_info.width - 20;
    if (win_h > fb_info.height - WM_DESKBAR_H - 20)
        win_h = fb_info.height - WM_DESKBAR_H - 20;

    int32_t win_x = ((int32_t)fb_info.width - (int32_t)win_w) / 2;
    int32_t win_y = (int32_t)WM_DESKBAR_H +
                    ((int32_t)(fb_info.height - WM_DESKBAR_H) - (int32_t)win_h) / 2;

    ed->win = wm_create_window(win_x, win_y, win_w, win_h, title);
    if (!ed->win) {
        ed->active = 0;
        return;
    }

    ed->win->repaint = ge_repaint_cb;

    /* Register File menu */
    wm_menu_t *file_menu = wm_window_add_menu(ed->win, "File");
    if (file_menu) {
        wm_menu_add_item(file_menu, "Save", ge_action_save, ed);
        wm_menu_add_item(file_menu, "Quit", ge_action_quit, ed);
    }

    /* Focus this window */
    wm_focus_window(ed->win);

    /* Load file */
    ge_load_file(ed);
    ed->cx = 0;
    ed->cy = 0;
    ed->scroll = 0;
    ed->modified = 0;
    ed->quit = 0;
    ed->status[0] = '\0';

    /* Initial draw */
    wm_redraw_all();

    /* Track previous mouse button state for click detection */
    int prev_lmb = 0;

    /* Main loop */
    while (!ed->quit) {
        wm_process_events();

        /* Check close dot */
        if (ed->win->flags & WIN_FLAG_CLOSE_REQ) {
            ed->quit = 1;
            break;
        }

        /* Mouse click-to-position cursor */
        {
            mouse_state_t ms = mouse_get_state();
            int cur_lmb = (ms.buttons & MOUSE_BTN_LEFT) ? 1 : 0;

            /* Detect left-button-down transition */
            if (cur_lmb && !prev_lmb && ed->win) {
                int32_t mx = ms.x;
                int32_t my = ms.y;
                int32_t cx = (int32_t)ed->win->content_x;
                int32_t cy = (int32_t)ed->win->content_y;
                int32_t cw = (int32_t)ed->win->content_w;
                int32_t ch = (int32_t)ed->win->content_h - FONT_H; /* exclude status bar */

                /* Check if click is within text content area */
                if (mx >= cx && mx < cx + cw && my >= cy && my < cy + ch) {
                    int col = (int)(mx - cx) / FONT_W;
                    int row = (int)(my - cy) / FONT_H;
                    int file_line = row + ed->scroll;

                    /* Clamp to valid line */
                    if (file_line >= ed->nlines)
                        file_line = ed->nlines - 1;
                    if (file_line < 0)
                        file_line = 0;

                    ed->cy = file_line;

                    /* Clamp column to line length */
                    if (col > ed->line_len[ed->cy])
                        col = ed->line_len[ed->cy];
                    if (col < 0)
                        col = 0;
                    ed->cx = col;

                    ed->status[0] = '\0';
                    ge_scroll_to_cursor(ed);
                    gui_editor_draw(ed);
                }
            }
            prev_lmb = cur_lmb;
        }

        /* Focus-gated keyboard */
        if (!(ed->win->flags & WIN_FLAG_FOCUSED)) {
            hal_halt();
            continue;
        }

        key_event_t k = keyboard_get_event();
        if (k.type == KEY_NONE) {
            hal_halt();
            continue;
        }

        /* Clear status on any key */
        ed->status[0] = '\0';

        int redraw = 1;
        switch (k.type) {
        case KEY_CHAR:
            ge_insert_char(ed, k.ch);
            break;
        case KEY_ENTER:
            ge_insert_newline(ed);
            break;
        case KEY_BACKSPACE:
            ge_backspace(ed);
            break;
        case KEY_DELETE:
            ge_delete(ed);
            break;
        case KEY_LEFT:
            if (ed->cx > 0) ed->cx--;
            else if (ed->cy > 0) { ed->cy--; ed->cx = ed->line_len[ed->cy]; }
            break;
        case KEY_RIGHT:
            if (ed->cx < ed->line_len[ed->cy]) ed->cx++;
            else if (ed->cy < ed->nlines - 1) { ed->cy++; ed->cx = 0; }
            break;
        case KEY_UP:
            if (ed->cy > 0) { ed->cy--; ge_clamp_cx(ed); }
            break;
        case KEY_DOWN:
            if (ed->cy < ed->nlines - 1) { ed->cy++; ge_clamp_cx(ed); }
            break;
        case KEY_HOME:
            ed->cx = 0;
            break;
        case KEY_END:
            ed->cx = ed->line_len[ed->cy];
            break;
        case KEY_PAGE_UP:
            ed->cy -= ed->text_rows;
            if (ed->cy < 0) ed->cy = 0;
            ge_clamp_cx(ed);
            break;
        case KEY_PAGE_DOWN:
            ed->cy += ed->text_rows;
            if (ed->cy >= ed->nlines) ed->cy = ed->nlines - 1;
            ge_clamp_cx(ed);
            break;
        case KEY_CTRL_S:
            ge_save_file(ed);
            break;
        case KEY_CTRL_X:
        case KEY_CTRL_C:
            ed->quit = 1;
            break;
        case KEY_CTRL_K:
            ge_cut_line(ed);
            break;
        default:
            redraw = 0;
            break;
        }

        if (redraw) {
            ge_scroll_to_cursor(ed);
            gui_editor_draw(ed);
        }
    }

    /* Cleanup */
    ge_free_lines(ed);
    window_t *w = ed->win;
    ed->win = NULL;
    ed->active = 0;
    wm_destroy_window(w);

    /* Refocus shell */
    window_t *sw = wm_get_shell_window();
    if (sw) {
        wm_focus_window(sw);
        wm_redraw_all();
    }

    /* Terminate this kernel thread — kernel threads must never return
       (there's no valid return address on the stack). */
    proc_kill(current_process->pid);
    for (;;) hal_halt();
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

void gui_editor_open(const char *filename) {
    /* Find a free editor slot */
    int slot = -1;
    for (int i = 0; i < MAX_GUI_EDITORS; i++) {
        if (!editors[i].active) { slot = i; break; }
    }
    if (slot < 0) return;  /* all slots in use */

    gui_editor_t *ed = &editors[slot];
    memset(ed, 0, sizeof(*ed));
    ed->active = 1;

    /* Copy filename */
    int i;
    for (i = 0; filename[i] && i < 126; i++)
        ed->filename[i] = filename[i];
    ed->filename[i] = '\0';

    /* Set pending slot and spawn thread */
    pending_slot = slot;
    proc_create_kernel_thread(gui_editor_thread);
}
