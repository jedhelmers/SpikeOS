#include <kernel/gui_editor.h>
#include <kernel/window.h>
#include <kernel/surface.h>
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
#include <kernel/timer.h>
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
#define GE_TAB_WIDTH         4
#define GE_TOOLBAR_H        24   /* toolbar height in pixels */
#define GE_MAX_UNDO        256   /* max undo/redo depth */
#define GE_DCLICK_TICKS     40   /* double-click threshold: 400ms at 100Hz */

/* Colors */
#define GE_FG      fb_pack_color(220, 220, 220)
#define GE_BG      fb_pack_color(0, 0, 0)
#define GE_BAR_FG  fb_pack_color(0, 0, 0)
#define GE_BAR_BG  fb_pack_color(200, 200, 200)
#define GE_CURSOR  fb_pack_color(220, 220, 220)
#define GE_SEL_FG  fb_pack_color(255, 255, 255)
#define GE_SEL_BG  fb_pack_color(50, 80, 140)

/* Toolbar colors */
#define GE_TB_BG   fb_pack_color(40, 40, 50)
#define GE_TB_FG   fb_pack_color(200, 200, 200)
#define GE_TB_SEP  fb_pack_color(70, 70, 85)

/* Toolbar button layout */
#define GE_TB_PAD_X  6
#define GE_TB_GAP    4

/* Scroll bar dimensions */
#define GE_VSCROLL_W        14   /* vertical scrollbar width in pixels */
#define GE_HSCROLL_H        14   /* horizontal scrollbar height in pixels */
#define GE_SCROLL_MIN_THUMB 20   /* minimum thumb length in pixels */
#define GE_SCROLL_LINES      3   /* lines per wheel notch */

/* Scroll bar colors (dark theme) */
#define GE_SB_TRACK    fb_pack_color(30, 30, 40)
#define GE_SB_THUMB    fb_pack_color(80, 80, 100)
#define GE_SB_THUMB_HL fb_pack_color(110, 110, 140)

/* ------------------------------------------------------------------ */
/*  Toolbar button definitions                                         */
/* ------------------------------------------------------------------ */

typedef enum {
    TB_CUT, TB_COPY, TB_PASTE,
    TB_SEP1,
    TB_ZOOM_IN, TB_ZOOM_OUT,
    TB_SEP2,
    TB_SAVE,
    TB_COUNT
} tb_button_id_t;

typedef struct {
    const char *label;   /* NULL for separator */
    int x, w;            /* pixel bounds within surface */
} tb_button_t;

static tb_button_t tb_buttons[TB_COUNT];

/* ------------------------------------------------------------------ */
/*  Command / undo-redo types                                          */
/* ------------------------------------------------------------------ */

typedef enum {
    CMD_INSERT,   /* text was inserted at (line, col) */
    CMD_DELETE,   /* text was deleted starting at (line, col) */
} cmd_type_t;

typedef struct {
    cmd_type_t type;
    int line, col;          /* start position */
    char *text;             /* inserted or deleted text (kmalloc'd, NUL-terminated) */
    int text_len;
    int old_cx, old_cy;    /* cursor before command */
    int new_cx, new_cy;    /* cursor after command */
} edit_cmd_t;

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

    int cx, cy;        /* cursor col, row in file (buffer positions) */
    int scroll;        /* first visible line */
    int text_rows;     /* rows available for text */
    int text_cols;     /* cols available for text (visual) */
    int modified;
    int quit;
    char status[GE_STATUS_MAX];

    int font_scale;    /* 1, 2, or 3 */

    /* Selection state */
    int sel_active;
    int sel_anchor_x;  /* buffer col of anchor */
    int sel_anchor_y;  /* line of anchor */

    int word_wrap;     /* 1 = wrap long lines, 0 = horizontal scroll */
    int hscroll;       /* horizontal scroll offset (visual cols, when !word_wrap) */
    int scroll_wrap;   /* wrap row within scroll line (when word_wrap) */

    /* Scroll bar interaction */
    int vscroll_dragging;
    int hscroll_dragging;
    int drag_start_mouse_y;
    int drag_start_mouse_x;
    int drag_start_scroll;
    int drag_start_hscroll;

    /* Undo/redo */
    edit_cmd_t undo_stack[GE_MAX_UNDO];
    int undo_count;
    edit_cmd_t redo_stack[GE_MAX_UNDO];
    int redo_count;
} gui_editor_t;

static gui_editor_t editors[MAX_GUI_EDITORS];
static int pending_slot = -1;

/* Global clipboard shared across editor instances */
static char *clipboard = NULL;
static int clipboard_len = 0;

/* Forward declarations (used by menu callbacks) */
static void ge_copy_selection(gui_editor_t *ed);
static void ge_cut_selection(gui_editor_t *ed);
static void ge_delete_selection(gui_editor_t *ed);
static void ge_paste(gui_editor_t *ed);
static void ge_select_all(gui_editor_t *ed);
static void ge_insert_char(gui_editor_t *ed, char c);
static void ge_insert_newline(gui_editor_t *ed);
static void ge_undo(gui_editor_t *ed);
static void ge_redo(gui_editor_t *ed);
static int  ge_save_as_dialog(gui_editor_t *ed);
static void gui_editor_draw(gui_editor_t *ed);
static void ge_draw_and_blit(gui_editor_t *ed);

/* ------------------------------------------------------------------ */
/*  Tab column conversion helpers                                      */
/* ------------------------------------------------------------------ */

/* Convert buffer column to visual column, expanding tabs */
static int buf_to_vcol(gui_editor_t *ed, int line, int buf_col) {
    if (line < 0 || line >= ed->nlines) return 0;
    int vcol = 0;
    const char *s = ed->lines[line];
    int len = ed->line_len[line];
    for (int i = 0; i < buf_col && i < len; i++) {
        if (s[i] == '\t')
            vcol += GE_TAB_WIDTH - (vcol % GE_TAB_WIDTH);
        else
            vcol++;
    }
    return vcol;
}

/* Convert visual column to buffer column (clamps to line length) */
static int vcol_to_buf(gui_editor_t *ed, int line, int target_vcol) {
    if (line < 0 || line >= ed->nlines) return 0;
    int vcol = 0;
    const char *s = ed->lines[line];
    int len = ed->line_len[line];
    for (int i = 0; i < len; i++) {
        int next_vcol;
        if (s[i] == '\t')
            next_vcol = vcol + GE_TAB_WIDTH - (vcol % GE_TAB_WIDTH);
        else
            next_vcol = vcol + 1;
        if (next_vcol > target_vcol) return i;
        vcol = next_vcol;
    }
    return len;
}

/* ------------------------------------------------------------------ */
/*  Word wrap helpers                                                   */
/* ------------------------------------------------------------------ */

/* How many visual rows does a line need in word wrap mode? */
static int ge_line_vrows(gui_editor_t *ed, int line) {
    if (!ed->word_wrap || ed->text_cols <= 0) return 1;
    if (line < 0 || line >= ed->nlines) return 1;
    int vcol = buf_to_vcol(ed, line, ed->line_len[line]);
    if (vcol == 0) return 1;
    return (vcol + ed->text_cols - 1) / ed->text_cols;
}

/* ------------------------------------------------------------------ */
/*  Selection helpers                                                   */
/* ------------------------------------------------------------------ */

/* Get selection range ordered (start <= end). Returns 0 if no selection. */
static int ge_get_selection(gui_editor_t *ed,
                            int *sy, int *sx, int *ey, int *ex) {
    if (!ed->sel_active) return 0;
    int ay = ed->sel_anchor_y, ax = ed->sel_anchor_x;
    int cy = ed->cy, cx = ed->cx;
    if (ay < cy || (ay == cy && ax <= cx)) {
        *sy = ay; *sx = ax; *ey = cy; *ex = cx;
    } else {
        *sy = cy; *sx = cx; *ey = ay; *ex = ax;
    }
    return 1;
}

/* Check if buffer position (line, col) is within the selection */
static int ge_in_selection(gui_editor_t *ed, int line, int col) {
    int sy, sx, ey, ex;
    if (!ge_get_selection(ed, &sy, &sx, &ey, &ex)) return 0;
    if (line < sy || line > ey) return 0;
    if (line == sy && line == ey) return col >= sx && col < ex;
    if (line == sy) return col >= sx;
    if (line == ey) return col < ex;
    return 1;  /* line between start and end */
}

/* ------------------------------------------------------------------ */
/*  Toolbar layout                                                     */
/* ------------------------------------------------------------------ */

static void ge_layout_toolbar(void) {
    static const char *labels[] = {
        "Cut", "Copy", "Paste", NULL, "A+", "A-", NULL, "Save"
    };
    int x = GE_TB_GAP;
    for (int i = 0; i < TB_COUNT; i++) {
        tb_buttons[i].label = labels[i];
        if (!labels[i]) {
            /* Separator */
            tb_buttons[i].x = x;
            tb_buttons[i].w = 2;
            x += 2 + GE_TB_GAP;
        } else {
            int lw = 0;
            for (const char *p = labels[i]; *p; p++) lw++;
            int btn_w = lw * FONT_W + 2 * GE_TB_PAD_X;
            tb_buttons[i].x = x;
            tb_buttons[i].w = btn_w;
            x += btn_w + GE_TB_GAP;
        }
    }
}

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

static int ge_is_untitled(gui_editor_t *ed) {
    return (strcmp(ed->filename, "/untitled") == 0);
}

/* Update the window title to reflect the current filename */
static void ge_update_title(gui_editor_t *ed) {
    if (!ed->win) return;
    const char *base = ed->filename;
    for (const char *p = ed->filename; *p; p++) {
        if (*p == '/') base = p + 1;
    }
    char *t = ed->win->title;
    int ti = 0;
    const char *pfx = "Edit: ";
    for (int i = 0; pfx[i] && ti < WIN_MAX_TITLE - 1; i++)
        t[ti++] = pfx[i];
    for (int i = 0; base[i] && ti < WIN_MAX_TITLE - 1; i++)
        t[ti++] = base[i];
    t[ti] = '\0';
}

/* Modal "Save As" dialog — draws over the editor surface, reads keyboard.
   Returns 0 on success (filename updated), -1 on cancel. */
static int ge_save_as_dialog(gui_editor_t *ed) {
    if (!ed->win || !ed->win->surface) return -1;
    surface_t *s = ed->win->surface;

    char buf[128];
    int blen = 0;

    /* Pre-fill with current filename (skip leading /) */
    const char *init = ed->filename;
    if (init[0] == '/') init++;
    if (strcmp(init, "untitled") != 0) {
        for (int i = 0; init[i] && blen < 126; i++)
            buf[blen++] = init[i];
    }
    buf[blen] = '\0';

    /* Dialog geometry */
    int dw = 320;
    int dh = 80;
    if (dw > (int)ed->win->content_w - 20) dw = (int)ed->win->content_w - 20;
    int dx = ((int)ed->win->content_w - dw) / 2;
    int dy = ((int)ed->win->content_h - dh) / 2;

    uint32_t dlg_bg    = fb_pack_color(50, 50, 65);
    uint32_t dlg_bord  = fb_pack_color(100, 100, 120);
    uint32_t input_bg  = fb_pack_color(20, 20, 30);
    uint32_t text_fg   = fb_pack_color(220, 220, 220);
    uint32_t hint_fg   = fb_pack_color(140, 140, 150);

    for (;;) {
        /* Draw dialog box */
        surface_fill_rect(s, (uint32_t)dx, (uint32_t)dy,
                          (uint32_t)dw, (uint32_t)dh, dlg_bg);
        /* Border */
        surface_draw_hline(s, (uint32_t)dx, (uint32_t)dy, (uint32_t)dw, dlg_bord);
        surface_draw_hline(s, (uint32_t)dx, (uint32_t)(dy + dh - 1), (uint32_t)dw, dlg_bord);
        for (int yy = dy; yy < dy + dh; yy++) {
            surface_putpixel(s, (uint32_t)dx, (uint32_t)yy, dlg_bord);
            surface_putpixel(s, (uint32_t)(dx + dw - 1), (uint32_t)yy, dlg_bord);
        }

        /* "Save As:" label */
        const char *label = "Save As:";
        int lx = dx + 12;
        int ly = dy + 10;
        for (int i = 0; label[i]; i++)
            surface_render_char(s, (uint32_t)(lx + i * FONT_W), (uint32_t)ly,
                                (uint8_t)label[i], text_fg, dlg_bg);

        /* Input field */
        int ix = dx + 12;
        int iy = dy + 10 + FONT_H + 8;
        int iw = dw - 24;
        int ih = FONT_H + 8;
        surface_fill_rect(s, (uint32_t)ix, (uint32_t)iy,
                          (uint32_t)iw, (uint32_t)ih, input_bg);

        /* Render filename text */
        int max_chars = (iw - 8) / FONT_W;
        int start = 0;
        if (blen > max_chars) start = blen - max_chars;
        for (int i = start; i < blen; i++) {
            surface_render_char(s, (uint32_t)(ix + 4 + (i - start) * FONT_W),
                                (uint32_t)(iy + 4),
                                (uint8_t)buf[i], text_fg, input_bg);
        }

        /* Cursor */
        int cursor_x = ix + 4 + (blen - start) * FONT_W;
        if (cursor_x < ix + iw - 2)
            surface_fill_rect(s, (uint32_t)cursor_x, (uint32_t)(iy + 4),
                              2, FONT_H, text_fg);

        /* Hint text */
        const char *hint = "Enter=save  Ctrl+Q=cancel";
        int hx = dx + 12;
        int hy = dy + dh - FONT_H - 4;
        /* Only draw hint if it fits below the input */
        if (hy > iy + ih) {
            for (int i = 0; hint[i]; i++)
                surface_render_char(s, (uint32_t)(hx + i * FONT_W), (uint32_t)hy,
                                    (uint8_t)hint[i], hint_fg, dlg_bg);
        }

        /* Blit */
        mouse_hide_cursor();
        surface_blit_to_fb(s, ed->win->content_x, ed->win->content_y);
        mouse_show_cursor();

        /* Process events (keep WM responsive) */
        wm_process_events();

        /* Read keyboard */
        key_event_t k = keyboard_get_event();
        if (k.type == KEY_NONE) {
            hal_halt();
            continue;
        }

        if (k.type == KEY_ENTER) {
            if (blen == 0) continue;  /* don't save empty name */
            /* Build full path: prepend / if not present */
            char path[128];
            int pi = 0;
            if (buf[0] != '/') path[pi++] = '/';
            for (int i = 0; i < blen && pi < 126; i++)
                path[pi++] = buf[i];
            path[pi] = '\0';
            /* Update editor filename */
            strcpy(ed->filename, path);
            ge_update_title(ed);
            return 0;
        }

        if (k.type == KEY_CTRL_Q || k.type == KEY_CTRL_X) {
            return -1;
        }

        if (k.type == KEY_BACKSPACE) {
            if (blen > 0) blen--;
            buf[blen] = '\0';
            continue;
        }

        if (k.type == KEY_CHAR && blen < 126) {
            char c = k.ch;
            /* Allow alphanumeric, dot, dash, underscore, slash */
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '.' || c == '-' ||
                c == '_' || c == '/') {
                buf[blen++] = c;
                buf[blen] = '\0';
            }
        }
    }
}

static int ge_save_file(gui_editor_t *ed) {
    /* If the file is untitled, prompt for a name first */
    if (ge_is_untitled(ed)) {
        if (ge_save_as_dialog(ed) < 0) {
            strcpy(ed->status, "Save cancelled");
            return -1;
        }
        /* Redraw with updated title */
        wm_draw_chrome(ed->win);
        ge_draw_and_blit(ed);
    }

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
    int fw = FONT_W * ed->font_scale;
    int fh = FONT_H * ed->font_scale;
    /* Subtract vertical scrollbar width from available text width */
    int avail_w = (int)ed->win->content_w - GE_VSCROLL_W;
    if (avail_w < fw) avail_w = fw;
    ed->text_cols = avail_w / fw;
    /* Subtract horizontal scrollbar height when not in word wrap mode */
    int hscroll_h = ed->word_wrap ? 0 : GE_HSCROLL_H;
    int avail_h = (int)ed->win->content_h - GE_TOOLBAR_H - FONT_H - hscroll_h;
    ed->text_rows = avail_h / fh;
    if (ed->text_rows < 1) ed->text_rows = 1;
}

/* ------------------------------------------------------------------ */
/*  Scroll bar geometry helpers                                        */
/* ------------------------------------------------------------------ */

/* Total visual rows in the document */
static int ge_total_vrows(gui_editor_t *ed) {
    if (ed->word_wrap) {
        int total = 0;
        for (int i = 0; i < ed->nlines; i++)
            total += ge_line_vrows(ed, i);
        return total;
    }
    return ed->nlines;
}

/* Current scroll position as a visual row offset from top */
static int ge_scroll_vrow(gui_editor_t *ed) {
    if (ed->word_wrap) {
        int vr = 0;
        for (int i = 0; i < ed->scroll; i++)
            vr += ge_line_vrows(ed, i);
        vr += ed->scroll_wrap;
        return vr;
    }
    return ed->scroll;
}

/* Longest line's visual column count */
static int ge_max_vcol(gui_editor_t *ed) {
    int max_vc = 0;
    for (int i = 0; i < ed->nlines; i++) {
        int vc = buf_to_vcol(ed, i, ed->line_len[i]);
        if (vc > max_vc) max_vc = vc;
    }
    return max_vc;
}

/* Vertical scrollbar track geometry (relative to surface) */
static void ge_vscroll_rect(gui_editor_t *ed,
                             int *track_x, int *track_y,
                             int *track_w, int *track_h) {
    int hscroll_h = ed->word_wrap ? 0 : GE_HSCROLL_H;
    *track_x = (int)ed->win->content_w - GE_VSCROLL_W;
    *track_y = GE_TOOLBAR_H;
    *track_w = GE_VSCROLL_W;
    *track_h = (int)ed->win->content_h - GE_TOOLBAR_H - FONT_H - hscroll_h;
}

/* Vertical thumb position and size within the track */
static void ge_vscroll_thumb(gui_editor_t *ed,
                              int track_y, int track_h,
                              int *thumb_y, int *thumb_h) {
    int total = ge_total_vrows(ed);
    if (total <= ed->text_rows) {
        *thumb_y = track_y;
        *thumb_h = track_h;
        return;
    }
    int th = (ed->text_rows * track_h) / total;
    if (th < GE_SCROLL_MIN_THUMB) th = GE_SCROLL_MIN_THUMB;
    int scroll_vr = ge_scroll_vrow(ed);
    int max_scroll = total - ed->text_rows;
    if (max_scroll < 1) max_scroll = 1;
    int ty = track_y + (scroll_vr * (track_h - th)) / max_scroll;
    if (ty < track_y) ty = track_y;
    if (ty + th > track_y + track_h) ty = track_y + track_h - th;
    *thumb_y = ty;
    *thumb_h = th;
}

/* Horizontal scrollbar track geometry (only when !word_wrap) */
static void ge_hscroll_rect(gui_editor_t *ed,
                              int *track_x, int *track_y,
                              int *track_w, int *track_h) {
    *track_x = 0;
    *track_y = (int)ed->win->content_h - FONT_H - GE_HSCROLL_H;
    *track_w = (int)ed->win->content_w - GE_VSCROLL_W;
    *track_h = GE_HSCROLL_H;
}

/* Horizontal thumb position and size */
static void ge_hscroll_thumb(gui_editor_t *ed,
                               int track_x, int track_w,
                               int *thumb_x, int *thumb_w) {
    int max_vc = ge_max_vcol(ed);
    if (max_vc <= ed->text_cols) {
        *thumb_x = track_x;
        *thumb_w = track_w;
        return;
    }
    int tw = (ed->text_cols * track_w) / max_vc;
    if (tw < GE_SCROLL_MIN_THUMB) tw = GE_SCROLL_MIN_THUMB;
    int max_hscroll = max_vc - ed->text_cols;
    if (max_hscroll < 1) max_hscroll = 1;
    int tx = track_x + (ed->hscroll * (track_w - tw)) / max_hscroll;
    if (tx < track_x) tx = track_x;
    if (tx + tw > track_x + track_w) tx = track_x + track_w - tw;
    *thumb_x = tx;
    *thumb_w = tw;
}

/* Scroll by N visual rows (positive = show later content) */
static void ge_scroll_by_vrows(gui_editor_t *ed, int delta) {
    if (ed->word_wrap) {
        int current_vr = ge_scroll_vrow(ed);
        int target_vr = current_vr + delta;
        int total = ge_total_vrows(ed);
        int max_vr = total - ed->text_rows;
        if (max_vr < 0) max_vr = 0;
        if (target_vr < 0) target_vr = 0;
        if (target_vr > max_vr) target_vr = max_vr;

        int vr = 0;
        for (int fl = 0; fl < ed->nlines; fl++) {
            int lv = ge_line_vrows(ed, fl);
            if (vr + lv > target_vr) {
                ed->scroll = fl;
                ed->scroll_wrap = target_vr - vr;
                return;
            }
            vr += lv;
        }
        ed->scroll = ed->nlines > 0 ? ed->nlines - 1 : 0;
        ed->scroll_wrap = 0;
    } else {
        ed->scroll += delta;
        if (ed->scroll < 0) ed->scroll = 0;
        int max_scroll = ed->nlines - ed->text_rows;
        if (max_scroll < 0) max_scroll = 0;
        if (ed->scroll > max_scroll) ed->scroll = max_scroll;
    }
}

/* Set scroll to a specific visual row (for thumb drag) */
static void ge_scroll_to_vrow(gui_editor_t *ed, int target_vr) {
    int total = ge_total_vrows(ed);
    int max_vr = total - ed->text_rows;
    if (max_vr < 0) max_vr = 0;
    if (target_vr < 0) target_vr = 0;
    if (target_vr > max_vr) target_vr = max_vr;

    if (ed->word_wrap) {
        int vr = 0;
        for (int fl = 0; fl < ed->nlines; fl++) {
            int lv = ge_line_vrows(ed, fl);
            if (vr + lv > target_vr) {
                ed->scroll = fl;
                ed->scroll_wrap = target_vr - vr;
                return;
            }
            vr += lv;
        }
        ed->scroll = ed->nlines > 0 ? ed->nlines - 1 : 0;
        ed->scroll_wrap = 0;
    } else {
        ed->scroll = target_vr;
    }
}

/* Vertical scrollbar hit-test: 0=miss, 1=above thumb, 2=on thumb, 3=below thumb */
static int ge_vscroll_hit(gui_editor_t *ed, int32_t mx, int32_t my) {
    if (!ed->win) return 0;
    int tx, ty, tw, th;
    ge_vscroll_rect(ed, &tx, &ty, &tw, &th);
    int rx = (int)(mx - (int32_t)ed->win->content_x);
    int ry = (int)(my - (int32_t)ed->win->content_y);
    if (rx < tx || rx >= tx + tw || ry < ty || ry >= ty + th)
        return 0;
    int thumb_y, thumb_h;
    ge_vscroll_thumb(ed, ty, th, &thumb_y, &thumb_h);
    if (ry < thumb_y) return 1;
    if (ry < thumb_y + thumb_h) return 2;
    return 3;
}

/* Horizontal scrollbar hit-test: 0=miss, 1=left of thumb, 2=on thumb, 3=right */
static int ge_hscroll_hit(gui_editor_t *ed, int32_t mx, int32_t my) {
    if (!ed->win || ed->word_wrap) return 0;
    int tx, ty, tw, th;
    ge_hscroll_rect(ed, &tx, &ty, &tw, &th);
    int rx = (int)(mx - (int32_t)ed->win->content_x);
    int ry = (int)(my - (int32_t)ed->win->content_y);
    if (rx < tx || rx >= tx + tw || ry < ty || ry >= ty + th)
        return 0;
    int thumb_x, thumb_w;
    ge_hscroll_thumb(ed, tx, tw, &thumb_x, &thumb_w);
    if (rx < thumb_x) return 1;
    if (rx < thumb_x + thumb_w) return 2;
    return 3;
}

static void ge_putchar_at(gui_editor_t *ed, int x, int y, char ch,
                           uint32_t fg, uint32_t bg) {
    if (!ed->win || !ed->win->surface) return;
    int fw = FONT_W * ed->font_scale;
    int fh = FONT_H * ed->font_scale;
    if (x < 0 || y < 0 || x >= ed->text_cols || y >= ed->text_rows) return;
    surface_render_char_scaled(ed->win->surface,
                                (uint32_t)(x * fw),
                                (uint32_t)(GE_TOOLBAR_H + y * fh),
                                (uint8_t)ch, fg, bg,
                                ed->font_scale);
}

/* Draw a string into the status bar at 1x scale (pixel-based) */
static void ge_status_str(gui_editor_t *ed, int px, int py,
                            const char *s, uint32_t fg, uint32_t bg) {
    if (!ed->win || !ed->win->surface) return;
    for (int i = 0; s[i]; i++) {
        int x = px + i * FONT_W;
        if (x + FONT_W > (int)ed->win->content_w) break;
        surface_render_char(ed->win->surface,
                            (uint32_t)x, (uint32_t)py,
                            (uint8_t)s[i], fg, bg);
    }
}

static void ge_draw_toolbar(gui_editor_t *ed) {
    if (!ed->win || !ed->win->surface) return;

    /* Background */
    surface_fill_rect(ed->win->surface, 0, 0,
                      ed->win->content_w, GE_TOOLBAR_H, GE_TB_BG);
    /* Bottom separator */
    surface_draw_hline(ed->win->surface, 0, GE_TOOLBAR_H - 1,
                       ed->win->content_w, GE_TB_SEP);

    int ty = (GE_TOOLBAR_H - FONT_H) / 2;

    for (int i = 0; i < TB_COUNT; i++) {
        if (!tb_buttons[i].label) {
            /* Draw separator */
            surface_fill_rect(ed->win->surface,
                              (uint32_t)tb_buttons[i].x, 4,
                              2, GE_TOOLBAR_H - 8, GE_TB_SEP);
        } else {
            /* Draw button label */
            int bx = tb_buttons[i].x + GE_TB_PAD_X;
            const char *lbl = tb_buttons[i].label;
            for (int c = 0; lbl[c]; c++) {
                surface_render_char(ed->win->surface,
                                    (uint32_t)(bx + c * FONT_W),
                                    (uint32_t)ty,
                                    (uint8_t)lbl[c], GE_TB_FG, GE_TB_BG);
            }
        }
    }
}

/* Map screen pixel (mx, my) to file position. Returns 1 if in text area. */
static int ge_screen_to_file(gui_editor_t *ed, int32_t mx, int32_t my,
                              int *out_line, int *out_col) {
    if (!ed->win) return 0;
    int fw = FONT_W * ed->font_scale;
    int fh = FONT_H * ed->font_scale;
    int32_t cx_px = (int32_t)ed->win->content_x;
    int32_t text_top = (int32_t)ed->win->content_y + GE_TOOLBAR_H;
    int32_t text_h = (int32_t)(ed->text_rows * fh);
    int32_t text_w = (int32_t)ed->win->content_w - GE_VSCROLL_W;

    if (mx < cx_px || mx >= cx_px + text_w ||
        my < text_top || my >= text_top + text_h)
        return 0;

    int click_col = (int)(mx - cx_px) / fw;
    if (click_col < 0) click_col = 0;
    int click_row = (int)(my - text_top) / fh;

    int file_line, vcol;

    if (ed->word_wrap) {
        int sr = 0;
        file_line = ed->nlines - 1;
        vcol = 0;
        for (int fl = ed->scroll; fl < ed->nlines; fl++) {
            int skip = (fl == ed->scroll) ? ed->scroll_wrap : 0;
            int lv = ge_line_vrows(ed, fl) - skip;
            if (sr + lv > click_row) {
                file_line = fl;
                int wr = click_row - sr + skip;
                vcol = wr * ed->text_cols + click_col;
                break;
            }
            sr += lv;
        }
    } else {
        file_line = click_row + ed->scroll;
        vcol = click_col + ed->hscroll;
    }

    if (file_line >= ed->nlines) file_line = ed->nlines - 1;
    if (file_line < 0) file_line = 0;

    int buf_col = vcol_to_buf(ed, file_line, vcol);
    if (buf_col > ed->line_len[file_line])
        buf_col = ed->line_len[file_line];

    *out_line = file_line;
    *out_col = buf_col;
    return 1;
}

static void gui_editor_draw(gui_editor_t *ed) {
    if (!ed->win || !ed->win->surface) return;

    ge_compute_dims(ed);
    uint32_t fg = GE_FG;
    uint32_t bg = GE_BG;

    /* Clear surface */
    surface_clear(ed->win->surface, bg);

    /* Draw toolbar */
    ge_draw_toolbar(ed);

    /* Draw text lines with tab expansion and selection highlighting */
    if (ed->word_wrap) {
        /* ---- Word wrap mode ---- */
        int screen_row = 0;
        for (int fl = ed->scroll; fl < ed->nlines && screen_row < ed->text_rows; fl++) {
            int skip = (fl == ed->scroll) ? ed->scroll_wrap : 0;
            int len = ed->line_len[fl];
            int vcol = 0;

            for (int bi = 0; bi < len; bi++) {
                char c = ed->lines[fl][bi];
                int selected = ge_in_selection(ed, fl, bi);
                uint32_t cfgc = selected ? GE_SEL_FG : fg;
                uint32_t cbgc = selected ? GE_SEL_BG : bg;

                if (c == '\t') {
                    int tab_end = vcol + GE_TAB_WIDTH - (vcol % GE_TAB_WIDTH);
                    while (vcol < tab_end) {
                        int wr = vcol / ed->text_cols;
                        int dc = vcol % ed->text_cols;
                        int sr = screen_row + wr - skip;
                        if (sr >= 0 && sr < ed->text_rows)
                            ge_putchar_at(ed, dc, sr, ' ', cfgc, cbgc);
                        vcol++;
                    }
                } else {
                    int wr = vcol / ed->text_cols;
                    int dc = vcol % ed->text_cols;
                    int sr = screen_row + wr - skip;
                    if (sr >= 0 && sr < ed->text_rows)
                        ge_putchar_at(ed, dc, sr, c, cfgc, cbgc);
                    vcol++;
                }
            }

            /* Selection highlight past EOL on the last wrap row */
            if (ed->sel_active) {
                int sy, sx, ey, ex;
                if (ge_get_selection(ed, &sy, &sx, &ey, &ex)) {
                    if (fl >= sy && fl < ey) {
                        int wr = (vcol > 0) ? (vcol - 1) / ed->text_cols : 0;
                        int dc = vcol % ed->text_cols;
                        int sr = screen_row + wr - skip;
                        if (dc == 0 && vcol > 0) { wr++; sr++; }
                        while (dc < ed->text_cols && sr >= 0 && sr < ed->text_rows) {
                            ge_putchar_at(ed, dc, sr, ' ', GE_SEL_FG, GE_SEL_BG);
                            dc++;
                        }
                    }
                }
            }

            int total_vrows = ge_line_vrows(ed, fl);
            screen_row += total_vrows - skip;
        }
    } else {
        /* ---- Horizontal scroll mode ---- */
        for (int row = 0; row < ed->text_rows; row++) {
            int file_line = ed->scroll + row;
            if (file_line >= ed->nlines) continue;

            int len = ed->line_len[file_line];
            int vcol = 0;
            for (int bi = 0; bi < len; bi++) {
                char c = ed->lines[file_line][bi];
                int selected = ge_in_selection(ed, file_line, bi);
                uint32_t cfgc = selected ? GE_SEL_FG : fg;
                uint32_t cbgc = selected ? GE_SEL_BG : bg;

                if (c == '\t') {
                    int tab_end = vcol + GE_TAB_WIDTH - (vcol % GE_TAB_WIDTH);
                    while (vcol < tab_end) {
                        int dc = vcol - ed->hscroll;
                        if (dc >= 0 && dc < ed->text_cols)
                            ge_putchar_at(ed, dc, row, ' ', cfgc, cbgc);
                        vcol++;
                    }
                } else {
                    int dc = vcol - ed->hscroll;
                    if (dc >= 0 && dc < ed->text_cols)
                        ge_putchar_at(ed, dc, row, c, cfgc, cbgc);
                    vcol++;
                }
            }

            /* Selection highlight past EOL */
            if (ed->sel_active) {
                int sy, sx, ey, ex;
                if (ge_get_selection(ed, &sy, &sx, &ey, &ex)) {
                    if (file_line >= sy && file_line < ey) {
                        int dc = vcol - ed->hscroll;
                        while (dc < ed->text_cols) {
                            if (dc >= 0)
                                ge_putchar_at(ed, dc, row, ' ', GE_SEL_FG, GE_SEL_BG);
                            dc++;
                            vcol++;
                        }
                    }
                }
            }
        }
    }

    /* Draw status bar at bottom (always 1x scale) */
    int status_py = (int)ed->win->content_h - FONT_H;
    surface_fill_rect(ed->win->surface, 0, (uint32_t)status_py,
                      ed->win->content_w, FONT_H, GE_BAR_BG);

    int status_cols = (int)(ed->win->content_w / FONT_W);

    /* Left: status message or filename */
    if (ed->status[0]) {
        ge_status_str(ed, FONT_W, status_py, ed->status, GE_BAR_FG, GE_BAR_BG);
    } else {
        ge_status_str(ed, FONT_W, status_py, ed->filename, GE_BAR_FG, GE_BAR_BG);
        if (ed->modified) {
            int flen = 0;
            while (ed->filename[flen]) flen++;
            ge_status_str(ed, FONT_W + (flen + 1) * FONT_W, status_py,
                         "[Modified]", GE_BAR_FG, GE_BAR_BG);
        }
    }

    /* Right: Ln X, Col Y (col is visual) */
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

    tmp = buf_to_vcol(ed, ed->cy, ed->cx) + 1;
    ni = 0;
    if (tmp == 0) num[ni++] = '0';
    else { while (tmp > 0) { num[ni++] = '0' + (tmp % 10); tmp /= 10; } }
    for (int i = ni - 1; i >= 0; i--) pos[pi++] = num[i];
    pos[pi] = '\0';

    int pos_x = (status_cols - pi - 1) * FONT_W;
    if (pos_x > 0)
        ge_status_str(ed, pos_x, status_py, pos, GE_BAR_FG, GE_BAR_BG);

    /* Draw scroll bars */
    {
        surface_t *s = ed->win->surface;

        /* --- Vertical scroll bar (always present) --- */
        {
            int tx, ty, tw, th;
            ge_vscroll_rect(ed, &tx, &ty, &tw, &th);
            surface_fill_rect(s, (uint32_t)tx, (uint32_t)ty,
                              (uint32_t)tw, (uint32_t)th, GE_SB_TRACK);
            /* 1px left separator */
            for (int yy = ty; yy < ty + th; yy++)
                surface_putpixel(s, (uint32_t)tx, (uint32_t)yy, GE_TB_SEP);
            /* Thumb */
            int thumb_y, thumb_h;
            ge_vscroll_thumb(ed, ty, th, &thumb_y, &thumb_h);
            uint32_t tc = ed->vscroll_dragging ? GE_SB_THUMB_HL : GE_SB_THUMB;
            surface_fill_rect(s, (uint32_t)(tx + 2), (uint32_t)thumb_y,
                              (uint32_t)(tw - 4), (uint32_t)thumb_h, tc);
        }

        /* --- Horizontal scroll bar (only when !word_wrap) --- */
        if (!ed->word_wrap) {
            int tx, ty, tw, th;
            ge_hscroll_rect(ed, &tx, &ty, &tw, &th);
            surface_fill_rect(s, (uint32_t)tx, (uint32_t)ty,
                              (uint32_t)tw, (uint32_t)th, GE_SB_TRACK);
            surface_draw_hline(s, (uint32_t)tx, (uint32_t)ty,
                              (uint32_t)tw, GE_TB_SEP);
            int thumb_x, thumb_w;
            ge_hscroll_thumb(ed, tx, tw, &thumb_x, &thumb_w);
            uint32_t tc = ed->hscroll_dragging ? GE_SB_THUMB_HL : GE_SB_THUMB;
            surface_fill_rect(s, (uint32_t)thumb_x, (uint32_t)(ty + 2),
                              (uint32_t)thumb_w, (uint32_t)(th - 4), tc);
        }

        /* Corner fill (junction of vscroll and hscroll) */
        if (!ed->word_wrap) {
            int cx = (int)ed->win->content_w - GE_VSCROLL_W;
            int cy_pos = (int)ed->win->content_h - FONT_H - GE_HSCROLL_H;
            surface_fill_rect(s, (uint32_t)cx, (uint32_t)cy_pos,
                              GE_VSCROLL_W, GE_HSCROLL_H, GE_SB_TRACK);
        }
    }

    /* Draw cursor (underline) */
    {
        int fw = FONT_W * ed->font_scale;
        int fh = FONT_H * ed->font_scale;
        int vcx = buf_to_vcol(ed, ed->cy, ed->cx);
        int cur_dc, cur_sr;

        if (ed->word_wrap) {
            cur_dc = vcx % ed->text_cols;
            int cursor_wr = vcx / ed->text_cols;
            /* Compute screen row for cursor */
            cur_sr = 0;
            for (int fl = ed->scroll; fl < ed->cy && fl < ed->nlines; fl++) {
                int skip = (fl == ed->scroll) ? ed->scroll_wrap : 0;
                cur_sr += ge_line_vrows(ed, fl) - skip;
            }
            cur_sr += cursor_wr - ((ed->cy == ed->scroll) ? ed->scroll_wrap : 0);
        } else {
            cur_dc = vcx - ed->hscroll;
            cur_sr = ed->cy - ed->scroll;
        }

        if (cur_dc >= 0 && cur_dc < ed->text_cols &&
            cur_sr >= 0 && cur_sr < ed->text_rows) {
            surface_fill_rect(ed->win->surface,
                              (uint32_t)(cur_dc * fw),
                              (uint32_t)(GE_TOOLBAR_H + cur_sr * fh + fh - 2),
                              (uint32_t)fw, 2, GE_CURSOR);
        }
    }
}

/* Repaint callback for the window manager (WM handles blitting) */
static void ge_repaint_cb(window_t *win) {
    for (int i = 0; i < MAX_GUI_EDITORS; i++) {
        if (editors[i].active && editors[i].win == win) {
            gui_editor_draw(&editors[i]);
            return;
        }
    }
}

/* Draw and immediately blit to framebuffer (for main-loop updates) */
static void ge_draw_and_blit(gui_editor_t *ed) {
    gui_editor_draw(ed);
    if (ed->win && ed->win->surface) {
        mouse_hide_cursor();
        surface_blit_to_fb(ed->win->surface,
                           ed->win->content_x, ed->win->content_y);
        mouse_show_cursor();
    }
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

    if (ed->word_wrap) {
        if (ed->text_cols <= 0) return;
        int vcx = buf_to_vcol(ed, ed->cy, ed->cx);
        int cursor_wr = vcx / ed->text_cols;

        /* Cursor's absolute visual row from top of file */
        int cursor_abs = 0;
        for (int fl = 0; fl < ed->cy; fl++)
            cursor_abs += ge_line_vrows(ed, fl);
        cursor_abs += cursor_wr;

        /* Scroll's absolute visual row */
        int scroll_abs = 0;
        for (int fl = 0; fl < ed->scroll; fl++)
            scroll_abs += ge_line_vrows(ed, fl);
        scroll_abs += ed->scroll_wrap;

        /* Scroll up if cursor is above viewport */
        if (cursor_abs < scroll_abs) {
            ed->scroll = ed->cy;
            ed->scroll_wrap = cursor_wr;
        }
        /* Scroll down if cursor is below viewport */
        else if (cursor_abs >= scroll_abs + ed->text_rows) {
            int target = cursor_abs - ed->text_rows + 1;
            int vr = 0;
            for (int fl = 0; fl < ed->nlines; fl++) {
                int lv = ge_line_vrows(ed, fl);
                if (vr + lv > target) {
                    ed->scroll = fl;
                    ed->scroll_wrap = target - vr;
                    break;
                }
                vr += lv;
            }
        }
    } else {
        /* Horizontal scroll: keep cursor visible */
        int vcx = buf_to_vcol(ed, ed->cy, ed->cx);
        if (vcx < ed->hscroll)
            ed->hscroll = vcx;
        if (vcx >= ed->hscroll + ed->text_cols)
            ed->hscroll = vcx - ed->text_cols + 1;

        /* Vertical scroll: file-line based */
        if (ed->cy < ed->scroll)
            ed->scroll = ed->cy;
        if (ed->cy >= ed->scroll + ed->text_rows)
            ed->scroll = ed->cy - ed->text_rows + 1;

        ed->scroll_wrap = 0;
    }
}

/* ------------------------------------------------------------------ */
/*  Undo/redo stack management                                         */
/* ------------------------------------------------------------------ */

static void ge_cmd_free(edit_cmd_t *cmd) {
    if (cmd->text) { kfree(cmd->text); cmd->text = NULL; }
    cmd->text_len = 0;
}

static void ge_clear_stack(edit_cmd_t *stack, int *count) {
    for (int i = 0; i < *count; i++)
        ge_cmd_free(&stack[i]);
    *count = 0;
}

static void ge_push_cmd(edit_cmd_t *stack, int *count, edit_cmd_t *cmd) {
    if (*count >= GE_MAX_UNDO) {
        ge_cmd_free(&stack[0]);
        for (int i = 0; i < GE_MAX_UNDO - 1; i++)
            stack[i] = stack[i + 1];
        (*count)--;
    }
    stack[*count] = *cmd;
    (*count)++;
}

/* ------------------------------------------------------------------ */
/*  Raw editing primitives (no undo tracking)                          */
/* ------------------------------------------------------------------ */

/* Insert text at (line, col). Returns end position via out params. */
static void ge_raw_insert(gui_editor_t *ed, int line, int col,
                           const char *text, int len,
                           int *end_line, int *end_col) {
    int cl = line, cc = col;
    for (int i = 0; i < len; i++) {
        if (text[i] == '\n') {
            if (ed->nlines >= GE_MAX_LINES) break;
            int tail_len = ed->line_len[cl] - cc;
            ge_insert_line(ed, cl + 1);
            if (tail_len > 0) {
                ge_line_ensure(ed, cl + 1, tail_len);
                memcpy(ed->lines[cl + 1], ed->lines[cl] + cc, (size_t)tail_len);
                ed->lines[cl + 1][tail_len] = '\0';
                ed->line_len[cl + 1] = tail_len;
            }
            ed->line_len[cl] = cc;
            ed->lines[cl][cc] = '\0';
            cl++;
            cc = 0;
        } else {
            ge_line_ensure(ed, cl, ed->line_len[cl] + 1);
            for (int j = ed->line_len[cl]; j > cc; j--)
                ed->lines[cl][j] = ed->lines[cl][j - 1];
            ed->lines[cl][cc] = text[i];
            ed->line_len[cl]++;
            ed->lines[cl][ed->line_len[cl]] = '\0';
            cc++;
        }
    }
    *end_line = cl;
    *end_col = cc;
}

/* Compute end position of text if inserted at (start_line, start_col) */
static void ge_text_end_pos(const char *text, int len,
                             int start_line, int start_col,
                             int *end_line, int *end_col) {
    int cl = start_line, cc = start_col;
    for (int i = 0; i < len; i++) {
        if (text[i] == '\n') { cl++; cc = 0; }
        else cc++;
    }
    *end_line = cl;
    *end_col = cc;
}

/* Extract text from range. Returns kmalloc'd NUL-terminated string. */
static char *ge_extract_text(gui_editor_t *ed, int sy, int sx,
                              int ey, int ex, int *out_len) {
    int total = 0;
    for (int line = sy; line <= ey; line++) {
        int start = (line == sy) ? sx : 0;
        int end   = (line == ey) ? ex : ed->line_len[line];
        total += end - start;
        if (line < ey) total++;
    }
    char *buf = (char *)kmalloc((size_t)(total + 1));
    if (!buf) { *out_len = 0; return NULL; }
    int pos = 0;
    for (int line = sy; line <= ey; line++) {
        int start = (line == sy) ? sx : 0;
        int end   = (line == ey) ? ex : ed->line_len[line];
        int len = end - start;
        if (len > 0) {
            memcpy(buf + pos, ed->lines[line] + start, (size_t)len);
            pos += len;
        }
        if (line < ey) buf[pos++] = '\n';
    }
    buf[pos] = '\0';
    *out_len = total;
    return buf;
}

/* Delete text from (sy,sx) to (ey,ex). Does NOT record undo. */
static void ge_raw_delete(gui_editor_t *ed, int sy, int sx, int ey, int ex) {
    if (sy == ey) {
        int dlen = ex - sx;
        if (dlen <= 0) return;
        for (int i = sx; i < ed->line_len[sy] - dlen; i++)
            ed->lines[sy][i] = ed->lines[sy][i + dlen];
        ed->line_len[sy] -= dlen;
        ed->lines[sy][ed->line_len[sy]] = '\0';
    } else {
        int tail_len = ed->line_len[ey] - ex;
        ge_line_ensure(ed, sy, sx + tail_len);
        if (tail_len > 0)
            memcpy(ed->lines[sy] + sx, ed->lines[ey] + ex, (size_t)tail_len);
        ed->line_len[sy] = sx + tail_len;
        ed->lines[sy][ed->line_len[sy]] = '\0';
        for (int i = ey; i > sy; i--)
            ge_delete_line(ed, i);
    }
}

/* ------------------------------------------------------------------ */
/*  Command-aware editing                                              */
/* ------------------------------------------------------------------ */

static void ge_do_insert(gui_editor_t *ed, int line, int col,
                          const char *text, int len) {
    int old_cx = ed->cx, old_cy = ed->cy;
    int end_line, end_col;
    ge_raw_insert(ed, line, col, text, len, &end_line, &end_col);

    ed->cy = end_line;
    ed->cx = end_col;
    ed->modified = 1;

    /* Try to merge with top of undo stack for consecutive char inserts */
    if (len == 1 && text[0] != '\n' && ed->undo_count > 0) {
        edit_cmd_t *top = &ed->undo_stack[ed->undo_count - 1];
        if (top->type == CMD_INSERT && top->text_len > 0 &&
            top->new_cy == line && top->new_cx == col &&
            top->text[top->text_len - 1] != '\n') {
            char *new_text = (char *)krealloc(top->text,
                                               (size_t)(top->text_len + 2));
            if (new_text) {
                new_text[top->text_len] = text[0];
                new_text[top->text_len + 1] = '\0';
                top->text = new_text;
                top->text_len++;
                top->new_cx = end_col;
                top->new_cy = end_line;
                ge_clear_stack(ed->redo_stack, &ed->redo_count);
                return;
            }
        }
    }

    edit_cmd_t cmd;
    cmd.type = CMD_INSERT;
    cmd.line = line;
    cmd.col = col;
    cmd.text = (char *)kmalloc((size_t)(len + 1));
    if (cmd.text) {
        memcpy(cmd.text, text, (size_t)len);
        cmd.text[len] = '\0';
    }
    cmd.text_len = len;
    cmd.old_cx = old_cx;
    cmd.old_cy = old_cy;
    cmd.new_cx = end_col;
    cmd.new_cy = end_line;

    ge_push_cmd(ed->undo_stack, &ed->undo_count, &cmd);
    ge_clear_stack(ed->redo_stack, &ed->redo_count);
}

static void ge_do_delete(gui_editor_t *ed, int sy, int sx, int ey, int ex) {
    int old_cx = ed->cx, old_cy = ed->cy;

    int tlen;
    char *text = ge_extract_text(ed, sy, sx, ey, ex, &tlen);
    ge_raw_delete(ed, sy, sx, ey, ex);

    ed->cy = sy;
    ed->cx = sx;
    ed->modified = 1;

    edit_cmd_t cmd;
    cmd.type = CMD_DELETE;
    cmd.line = sy;
    cmd.col = sx;
    cmd.text = text;
    cmd.text_len = tlen;
    cmd.old_cx = old_cx;
    cmd.old_cy = old_cy;
    cmd.new_cx = sx;
    cmd.new_cy = sy;

    ge_push_cmd(ed->undo_stack, &ed->undo_count, &cmd);
    ge_clear_stack(ed->redo_stack, &ed->redo_count);
}

/* ------------------------------------------------------------------ */
/*  Undo / Redo                                                        */
/* ------------------------------------------------------------------ */

static void ge_undo(gui_editor_t *ed) {
    if (ed->undo_count == 0) {
        strcpy(ed->status, "Nothing to undo");
        return;
    }

    edit_cmd_t cmd = ed->undo_stack[--ed->undo_count];

    if (cmd.type == CMD_INSERT) {
        ge_raw_delete(ed, cmd.line, cmd.col, cmd.new_cy, cmd.new_cx);
    } else {
        int end_line, end_col;
        ge_raw_insert(ed, cmd.line, cmd.col, cmd.text, cmd.text_len,
                       &end_line, &end_col);
    }

    ed->cy = cmd.old_cy;
    ed->cx = cmd.old_cx;
    ed->modified = 1;
    ed->sel_active = 0;

    ge_push_cmd(ed->redo_stack, &ed->redo_count, &cmd);
    strcpy(ed->status, "Undo");
}

static void ge_redo(gui_editor_t *ed) {
    if (ed->redo_count == 0) {
        strcpy(ed->status, "Nothing to redo");
        return;
    }

    edit_cmd_t cmd = ed->redo_stack[--ed->redo_count];

    if (cmd.type == CMD_INSERT) {
        int end_line, end_col;
        ge_raw_insert(ed, cmd.line, cmd.col, cmd.text, cmd.text_len,
                       &end_line, &end_col);
        ed->cy = cmd.new_cy;
        ed->cx = cmd.new_cx;
    } else {
        int end_line, end_col;
        ge_text_end_pos(cmd.text, cmd.text_len, cmd.line, cmd.col,
                         &end_line, &end_col);
        ge_raw_delete(ed, cmd.line, cmd.col, end_line, end_col);
        ed->cy = cmd.new_cy;
        ed->cx = cmd.new_cx;
    }

    ed->modified = 1;
    ed->sel_active = 0;

    ge_push_cmd(ed->undo_stack, &ed->undo_count, &cmd);
    strcpy(ed->status, "Redo");
}

/* ------------------------------------------------------------------ */
/*  Text editing (command-aware wrappers)                              */
/* ------------------------------------------------------------------ */

static void ge_insert_char(gui_editor_t *ed, char c) {
    ge_do_insert(ed, ed->cy, ed->cx, &c, 1);
}

static void ge_insert_newline(gui_editor_t *ed) {
    ge_do_insert(ed, ed->cy, ed->cx, "\n", 1);
}

static void ge_backspace(gui_editor_t *ed) {
    if (ed->cx > 0) {
        ge_do_delete(ed, ed->cy, ed->cx - 1, ed->cy, ed->cx);
    } else if (ed->cy > 0) {
        int prev_len = ed->line_len[ed->cy - 1];
        ge_do_delete(ed, ed->cy - 1, prev_len, ed->cy, 0);
    }
}

static void ge_delete(gui_editor_t *ed) {
    if (ed->cx < ed->line_len[ed->cy]) {
        ge_do_delete(ed, ed->cy, ed->cx, ed->cy, ed->cx + 1);
    } else if (ed->cy < ed->nlines - 1) {
        ge_do_delete(ed, ed->cy, ed->cx, ed->cy + 1, 0);
    }
}

static void ge_cut_line(gui_editor_t *ed) {
    /* Copy line text to clipboard */
    if (clipboard) { kfree(clipboard); clipboard = NULL; clipboard_len = 0; }
    int len = ed->line_len[ed->cy];
    clipboard = (char *)kmalloc((size_t)(len + 1));
    if (clipboard) {
        if (len > 0) memcpy(clipboard, ed->lines[ed->cy], (size_t)len);
        clipboard[len] = '\0';
        clipboard_len = len;
    }

    /* Delete the line via command system */
    if (ed->nlines == 1) {
        if (ed->line_len[0] > 0)
            ge_do_delete(ed, 0, 0, 0, ed->line_len[0]);
    } else if (ed->cy < ed->nlines - 1) {
        ge_do_delete(ed, ed->cy, 0, ed->cy + 1, 0);
    } else {
        ge_do_delete(ed, ed->cy - 1, ed->line_len[ed->cy - 1],
                      ed->cy, ed->line_len[ed->cy]);
    }
    ge_clamp_cx(ed);
}

/* ------------------------------------------------------------------ */
/*  Selection operations                                               */
/* ------------------------------------------------------------------ */

static void ge_copy_selection(gui_editor_t *ed) {
    int sy, sx, ey, ex;
    if (!ge_get_selection(ed, &sy, &sx, &ey, &ex)) return;

    if (clipboard) { kfree(clipboard); clipboard = NULL; clipboard_len = 0; }

    clipboard = ge_extract_text(ed, sy, sx, ey, ex, &clipboard_len);
    strcpy(ed->status, "Copied");
}

static void ge_delete_selection(gui_editor_t *ed) {
    int sy, sx, ey, ex;
    if (!ge_get_selection(ed, &sy, &sx, &ey, &ex)) return;
    ed->sel_active = 0;
    ge_do_delete(ed, sy, sx, ey, ex);
}

static void ge_cut_selection(gui_editor_t *ed) {
    ge_copy_selection(ed);
    ge_delete_selection(ed);
    strcpy(ed->status, "Cut");
}

static void ge_paste(gui_editor_t *ed) {
    if (!clipboard || clipboard_len == 0) return;
    if (ed->sel_active) ge_delete_selection(ed);
    ge_do_insert(ed, ed->cy, ed->cx, clipboard, clipboard_len);
    strcpy(ed->status, "Pasted");
}

static void ge_select_all(gui_editor_t *ed) {
    ed->sel_active = 1;
    ed->sel_anchor_x = 0;
    ed->sel_anchor_y = 0;
    ed->cy = ed->nlines - 1;
    ed->cx = ed->line_len[ed->cy];
}

/* ------------------------------------------------------------------ */
/*  Menu action callbacks                                              */
/* ------------------------------------------------------------------ */

static void ge_action_save(void *ctx) {
    ge_save_file((gui_editor_t *)ctx);
}

static void ge_action_save_as(void *ctx) {
    gui_editor_t *ed = (gui_editor_t *)ctx;
    if (ge_save_as_dialog(ed) == 0) {
        wm_draw_chrome(ed->win);
        ge_save_file(ed);   /* filename was updated, no longer untitled */
    } else {
        strcpy(ed->status, "Save cancelled");
    }
    ge_draw_and_blit(ed);
}

static void ge_action_quit(void *ctx) {
    ((gui_editor_t *)ctx)->quit = 1;
}

static void ge_action_cut(void *ctx) {
    ge_cut_selection((gui_editor_t *)ctx);
}

static void ge_action_copy(void *ctx) {
    ge_copy_selection((gui_editor_t *)ctx);
}

static void ge_action_paste(void *ctx) {
    ge_paste((gui_editor_t *)ctx);
}

static void ge_action_select_all(void *ctx) {
    ge_select_all((gui_editor_t *)ctx);
}

static void ge_action_zoom_in(void *ctx) {
    gui_editor_t *ed = (gui_editor_t *)ctx;
    if (ed->font_scale < 3) { ed->font_scale++; ge_compute_dims(ed); }
}

static void ge_action_zoom_out(void *ctx) {
    gui_editor_t *ed = (gui_editor_t *)ctx;
    if (ed->font_scale > 1) { ed->font_scale--; ge_compute_dims(ed); }
}

static void ge_action_toggle_wrap(void *ctx) {
    gui_editor_t *ed = (gui_editor_t *)ctx;
    ed->word_wrap = !ed->word_wrap;
    if (ed->word_wrap) {
        ed->hscroll = 0;
    } else {
        ed->scroll_wrap = 0;
    }
    ge_compute_dims(ed);
}

static void ge_action_undo(void *ctx) {
    ge_undo((gui_editor_t *)ctx);
}

static void ge_action_redo(void *ctx) {
    ge_redo((gui_editor_t *)ctx);
}

/* ------------------------------------------------------------------ */
/*  Selection tracking helpers for shift+movement                      */
/* ------------------------------------------------------------------ */

/* Call before a cursor movement key. Starts selection if shift is held. */
static void ge_sel_before_move(gui_editor_t *ed) {
    int shift = keyboard_shift_held();
    if (shift && !ed->sel_active) {
        ed->sel_active = 1;
        ed->sel_anchor_x = ed->cx;
        ed->sel_anchor_y = ed->cy;
    }
    if (!shift) {
        ed->sel_active = 0;
    }
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

    /* Register menus */
    wm_menu_t *file_menu = wm_window_add_menu(ed->win, "File");
    if (file_menu) {
        wm_menu_add_item(file_menu, "Save", ge_action_save, ed);
        wm_menu_add_item(file_menu, "Save As", ge_action_save_as, ed);
        wm_menu_add_item(file_menu, "Quit", ge_action_quit, ed);
    }

    wm_menu_t *edit_menu = wm_window_add_menu(ed->win, "Edit");
    if (edit_menu) {
        wm_menu_add_item(edit_menu, "Cut", ge_action_cut, ed);
        wm_menu_add_item(edit_menu, "Copy", ge_action_copy, ed);
        wm_menu_add_item(edit_menu, "Paste", ge_action_paste, ed);
        wm_menu_add_item(edit_menu, "Select All", ge_action_select_all, ed);
        wm_menu_add_item(edit_menu, "Undo", ge_action_undo, ed);
        wm_menu_add_item(edit_menu, "Redo", ge_action_redo, ed);
    }

    wm_menu_t *view_menu = wm_window_add_menu(ed->win, "View");
    if (view_menu) {
        wm_menu_add_item(view_menu, "Zoom In", ge_action_zoom_in, ed);
        wm_menu_add_item(view_menu, "Zoom Out", ge_action_zoom_out, ed);
        wm_menu_add_item(view_menu, "Toggle Wrap", ge_action_toggle_wrap, ed);
    }

    /* Focus this window */
    wm_focus_window(ed->win);

    /* Load file and initialize state */
    ge_load_file(ed);
    ed->cx = 0;
    ed->cy = 0;
    ed->scroll = 0;
    ed->modified = 0;
    ed->quit = 0;
    ed->status[0] = '\0';
    ed->font_scale = 1;
    ed->sel_active = 0;
    ed->word_wrap = 1;
    ed->hscroll = 0;
    ed->scroll_wrap = 0;
    ed->vscroll_dragging = 0;
    ed->hscroll_dragging = 0;

    ge_layout_toolbar();

    /* Initial draw */
    wm_redraw_all();

    /* Track mouse state for click and drag selection */
    int prev_lmb = 0;
    int mouse_selecting = 0;  /* 1 while LMB held after click in text area */

    /* Double/triple click tracking */
    uint32_t last_click_tick = 0;
    int last_click_line = -1;
    int last_click_col  = -1;
    int click_count = 0;  /* 1=single, 2=double, 3=triple */

    /* Main loop */
    while (!ed->quit) {
        wm_process_events();

        /* Check close dot */
        if (ed->win->flags & WIN_FLAG_CLOSE_REQ) {
            ed->quit = 1;
            break;
        }

        /* Consume scroll wheel accumulator */
        if (ed->win->scroll_accum != 0 &&
            (ed->win->flags & WIN_FLAG_FOCUSED)) {
            int32_t dz = ed->win->scroll_accum;
            ed->win->scroll_accum = 0;
            /* positive accum = scroll up, but ge positive = show later (down) */
            ge_scroll_by_vrows(ed, -dz * GE_SCROLL_LINES);
            ge_draw_and_blit(ed);
        }

        /* Mouse handling */
        {
            mouse_state_t ms = mouse_get_state();
            int cur_lmb = (ms.buttons & MOUSE_BTN_LEFT) ? 1 : 0;
            int32_t mx = ms.x;
            int32_t my = ms.y;

            if (cur_lmb && !prev_lmb && ed->win &&
                (ed->win->flags & WIN_FLAG_FOCUSED)) {
                /* Left-button-down transition */
                int32_t cx_px = (int32_t)ed->win->content_x;
                int32_t cy_px = (int32_t)ed->win->content_y;
                int32_t cw = (int32_t)ed->win->content_w;

                /* Check toolbar click */
                int32_t tb_top = cy_px;
                int32_t tb_bot = cy_px + GE_TOOLBAR_H;
                if (mx >= cx_px && mx < cx_px + cw &&
                    my >= tb_top && my < tb_bot) {
                    int rel_x = (int)(mx - cx_px);
                    for (int i = 0; i < TB_COUNT; i++) {
                        if (tb_buttons[i].label &&
                            rel_x >= tb_buttons[i].x &&
                            rel_x < tb_buttons[i].x + tb_buttons[i].w) {
                            switch (i) {
                                case TB_CUT:      ge_cut_selection(ed); break;
                                case TB_COPY:     ge_copy_selection(ed); break;
                                case TB_PASTE:    ge_paste(ed); break;
                                case TB_ZOOM_IN:
                                    if (ed->font_scale < 3) {
                                        ed->font_scale++;
                                        ge_compute_dims(ed);
                                    }
                                    break;
                                case TB_ZOOM_OUT:
                                    if (ed->font_scale > 1) {
                                        ed->font_scale--;
                                        ge_compute_dims(ed);
                                    }
                                    break;
                                case TB_SAVE:     ge_save_file(ed); break;
                                default: break;
                            }
                            ge_scroll_to_cursor(ed);
                            ge_draw_and_blit(ed);
                            break;
                        }
                    }
                    prev_lmb = cur_lmb;
                    goto next_iter;
                }

                /* Check vertical scrollbar click */
                {
                    int vhit = ge_vscroll_hit(ed, mx, my);
                    if (vhit == 1) {
                        ge_scroll_by_vrows(ed, -ed->text_rows);
                        ge_draw_and_blit(ed);
                        prev_lmb = cur_lmb;
                        goto next_iter;
                    } else if (vhit == 3) {
                        ge_scroll_by_vrows(ed, ed->text_rows);
                        ge_draw_and_blit(ed);
                        prev_lmb = cur_lmb;
                        goto next_iter;
                    } else if (vhit == 2) {
                        ed->vscroll_dragging = 1;
                        ed->drag_start_mouse_y = (int)my;
                        ed->drag_start_scroll = ge_scroll_vrow(ed);
                        ge_draw_and_blit(ed);
                        prev_lmb = cur_lmb;
                        goto next_iter;
                    }
                }

                /* Check horizontal scrollbar click */
                if (!ed->word_wrap) {
                    int hhit = ge_hscroll_hit(ed, mx, my);
                    if (hhit == 1) {
                        ed->hscroll -= ed->text_cols;
                        if (ed->hscroll < 0) ed->hscroll = 0;
                        ge_draw_and_blit(ed);
                        prev_lmb = cur_lmb;
                        goto next_iter;
                    } else if (hhit == 3) {
                        int max_vc = ge_max_vcol(ed);
                        ed->hscroll += ed->text_cols;
                        int max_hs = max_vc - ed->text_cols;
                        if (max_hs < 0) max_hs = 0;
                        if (ed->hscroll > max_hs) ed->hscroll = max_hs;
                        ge_draw_and_blit(ed);
                        prev_lmb = cur_lmb;
                        goto next_iter;
                    } else if (hhit == 2) {
                        ed->hscroll_dragging = 1;
                        ed->drag_start_mouse_x = (int)mx;
                        ed->drag_start_hscroll = ed->hscroll;
                        ge_draw_and_blit(ed);
                        prev_lmb = cur_lmb;
                        goto next_iter;
                    }
                }

                /* Check text area click */
                {
                    int click_line, click_col;
                    if (ge_screen_to_file(ed, mx, my, &click_line, &click_col)) {
                        uint32_t now = timer_ticks();

                        /* Detect multi-click (same position, within threshold) */
                        if (click_line == last_click_line &&
                            click_col == last_click_col &&
                            (now - last_click_tick) < GE_DCLICK_TICKS) {
                            click_count++;
                            if (click_count > 3) click_count = 3;
                        } else {
                            click_count = 1;
                        }
                        last_click_tick = now;
                        last_click_line = click_line;
                        last_click_col  = click_col;

                        ed->cy = click_line;
                        ed->cx = click_col;

                        if (click_count == 2) {
                            /* Double-click: select word */
                            const char *line = ed->lines[ed->cy];
                            int len = ed->line_len[ed->cy];
                            int wstart = ed->cx, wend = ed->cx;
                            /* Scan backwards for word start */
                            while (wstart > 0 && line[wstart - 1] != ' ' &&
                                   line[wstart - 1] != '\t')
                                wstart--;
                            /* Scan forwards for word end */
                            while (wend < len && line[wend] != ' ' &&
                                   line[wend] != '\t')
                                wend++;
                            ed->sel_active = 1;
                            ed->sel_anchor_x = wstart;
                            ed->sel_anchor_y = ed->cy;
                            ed->cx = wend;
                            mouse_selecting = 0;
                        } else if (click_count == 3) {
                            /* Triple-click: select entire line */
                            ed->sel_active = 1;
                            ed->sel_anchor_x = 0;
                            ed->sel_anchor_y = ed->cy;
                            ed->cx = ed->line_len[ed->cy];
                            mouse_selecting = 0;
                        } else {
                            /* Single click */
                            ed->sel_active = 0;
                            mouse_selecting = 1;
                            ed->sel_anchor_x = ed->cx;
                            ed->sel_anchor_y = ed->cy;
                        }

                        ed->status[0] = '\0';
                        ge_scroll_to_cursor(ed);
                        ge_draw_and_blit(ed);
                    }
                }
            } else if (cur_lmb && ed->win) {
                if (ed->vscroll_dragging) {
                    /* Vertical thumb drag */
                    int tx, ty, tw, th;
                    ge_vscroll_rect(ed, &tx, &ty, &tw, &th);
                    int thumb_y, thumb_h;
                    ge_vscroll_thumb(ed, ty, th, &thumb_y, &thumb_h);
                    int total = ge_total_vrows(ed);
                    int max_vr = total - ed->text_rows;
                    if (max_vr < 1) max_vr = 1;
                    int usable_track = th - thumb_h;
                    if (usable_track < 1) usable_track = 1;
                    int mouse_dy = (int)my - ed->drag_start_mouse_y;
                    int new_vr = ed->drag_start_scroll +
                                 (mouse_dy * max_vr) / usable_track;
                    ge_scroll_to_vrow(ed, new_vr);
                    ge_draw_and_blit(ed);
                } else if (ed->hscroll_dragging) {
                    /* Horizontal thumb drag */
                    int tx, ty, tw, th;
                    ge_hscroll_rect(ed, &tx, &ty, &tw, &th);
                    int thumb_x, thumb_w;
                    ge_hscroll_thumb(ed, tx, tw, &thumb_x, &thumb_w);
                    int max_vc = ge_max_vcol(ed);
                    int max_hs = max_vc - ed->text_cols;
                    if (max_hs < 1) max_hs = 1;
                    int usable_track = tw - thumb_w;
                    if (usable_track < 1) usable_track = 1;
                    int mouse_dx = (int)mx - ed->drag_start_mouse_x;
                    int new_hs = ed->drag_start_hscroll +
                                 (mouse_dx * max_hs) / usable_track;
                    if (new_hs < 0) new_hs = 0;
                    if (new_hs > max_hs) new_hs = max_hs;
                    ed->hscroll = new_hs;
                    ge_draw_and_blit(ed);
                } else if (mouse_selecting) {
                    /* Mouse drag for text selection */
                    int drag_line, drag_col;
                    if (ge_screen_to_file(ed, mx, my, &drag_line, &drag_col)) {
                        if (drag_line != ed->cy || drag_col != ed->cx) {
                            ed->sel_active = 1;
                            ed->cy = drag_line;
                            ed->cx = drag_col;
                            ge_scroll_to_cursor(ed);
                            ge_draw_and_blit(ed);
                        }
                    }
                }
            }

            if (!cur_lmb) {
                mouse_selecting = 0;
                ed->vscroll_dragging = 0;
                ed->hscroll_dragging = 0;
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
            if (ed->sel_active) ge_delete_selection(ed);
            ge_insert_char(ed, k.ch);
            break;
        case KEY_TAB:
            if (ed->sel_active) ge_delete_selection(ed);
            ge_insert_char(ed, '\t');
            break;
        case KEY_ENTER:
            if (ed->sel_active) ge_delete_selection(ed);
            ge_insert_newline(ed);
            break;
        case KEY_BACKSPACE:
            if (ed->sel_active) ge_delete_selection(ed);
            else ge_backspace(ed);
            break;
        case KEY_DELETE:
            if (ed->sel_active) ge_delete_selection(ed);
            else ge_delete(ed);
            break;
        case KEY_LEFT:
            ge_sel_before_move(ed);
            if (ed->cx > 0) ed->cx--;
            else if (ed->cy > 0) { ed->cy--; ed->cx = ed->line_len[ed->cy]; }
            break;
        case KEY_RIGHT:
            ge_sel_before_move(ed);
            if (ed->cx < ed->line_len[ed->cy]) ed->cx++;
            else if (ed->cy < ed->nlines - 1) { ed->cy++; ed->cx = 0; }
            break;
        case KEY_UP:
            ge_sel_before_move(ed);
            if (ed->cy > 0) { ed->cy--; ge_clamp_cx(ed); }
            break;
        case KEY_DOWN:
            ge_sel_before_move(ed);
            if (ed->cy < ed->nlines - 1) { ed->cy++; ge_clamp_cx(ed); }
            break;
        case KEY_HOME:
            ge_sel_before_move(ed);
            ed->cx = 0;
            break;
        case KEY_END:
            ge_sel_before_move(ed);
            ed->cx = ed->line_len[ed->cy];
            break;
        case KEY_PAGE_UP:
            ge_sel_before_move(ed);
            ed->cy -= ed->text_rows;
            if (ed->cy < 0) ed->cy = 0;
            ge_clamp_cx(ed);
            break;
        case KEY_PAGE_DOWN:
            ge_sel_before_move(ed);
            ed->cy += ed->text_rows;
            if (ed->cy >= ed->nlines) ed->cy = ed->nlines - 1;
            ge_clamp_cx(ed);
            break;
        case KEY_CTRL_S:
            ge_save_file(ed);
            break;
        case KEY_CTRL_C:
            ge_copy_selection(ed);
            break;
        case KEY_CTRL_X:
            ge_cut_selection(ed);
            break;
        case KEY_CTRL_V:
            ge_paste(ed);
            break;
        case KEY_CTRL_A:
            ge_select_all(ed);
            break;
        case KEY_CTRL_Q:
            ed->quit = 1;
            break;
        case KEY_CTRL_K:
            ge_cut_line(ed);
            break;
        case KEY_CTRL_PLUS:
            if (ed->font_scale < 3) {
                ed->font_scale++;
                ge_compute_dims(ed);
                ge_clamp_cx(ed);
            }
            break;
        case KEY_CTRL_MINUS:
            if (ed->font_scale > 1) {
                ed->font_scale--;
                ge_compute_dims(ed);
                ge_clamp_cx(ed);
            }
            break;
        case KEY_CTRL_Z:
            ge_undo(ed);
            break;
        case KEY_CTRL_SHIFT_Z:
            ge_redo(ed);
            break;
        default:
            redraw = 0;
            break;
        }

        if (redraw) {
            ge_scroll_to_cursor(ed);
            ge_draw_and_blit(ed);
        }

        continue;
next_iter:
        ;
    }

    /* Cleanup */
    ge_clear_stack(ed->undo_stack, &ed->undo_count);
    ge_clear_stack(ed->redo_stack, &ed->redo_count);
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

    /* Terminate this kernel thread */
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
