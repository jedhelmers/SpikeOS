#include <kernel/editor.h>
#include <kernel/keyboard.h>
#include <kernel/key_event.h>
#include <kernel/tty.h>
#include <kernel/fb_console.h>
#include <kernel/framebuffer.h>
#include <kernel/window.h>
#include <kernel/vfs.h>
#include <kernel/heap.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/*  Constants                                                          */
/* ------------------------------------------------------------------ */

#define ED_MAX_LINES     1024
#define ED_INIT_LINE_CAP  128
#define ED_FILENAME_MAX    64
#define ED_STATUS_MAX      80

/* VGA color indices */
#define COL_FG       15  /* white */
#define COL_BG        0  /* black */
#define COL_BAR_FG    0  /* black (for title/status/help bars) */
#define COL_BAR_BG   15  /* white */
#define COL_HELP_FG   0  /* black */
#define COL_HELP_BG   7  /* light gray */

/* ------------------------------------------------------------------ */
/*  Editor state                                                       */
/* ------------------------------------------------------------------ */

static char  *ed_lines[ED_MAX_LINES];   /* kmalloc'd line content */
static int    ed_len[ED_MAX_LINES];     /* length of each line */
static int    ed_cap[ED_MAX_LINES];     /* allocated capacity */
static int    ed_nlines;                /* number of lines */

static int    ed_cx, ed_cy;             /* cursor col, row in file */
static int    ed_scroll;                /* first visible line */
static int    ed_scr_rows, ed_scr_cols; /* screen dimensions */
static int    ed_text_rows;             /* rows available for text */

static char   ed_filename[ED_FILENAME_MAX];
static int    ed_modified;
static char   ed_status[ED_STATUS_MAX];
static int    ed_use_fb;                /* 1 = framebuffer, 0 = VGA */

static window_t *ed_win;               /* shell window (FB mode) */

/* ------------------------------------------------------------------ */
/*  Low-level rendering                                                */
/* ------------------------------------------------------------------ */

static uint8_t vga_color(uint8_t fg, uint8_t bg) {
    return (bg << 4) | (fg & 0x0F);
}

static void ed_putchar_at(int x, int y, char ch, uint8_t fg, uint8_t bg) {
    if (x < 0 || y < 0 || x >= ed_scr_cols || y >= ed_scr_rows)
        return;

    if (ed_use_fb && ed_win) {
        uint32_t px = ed_win->content_x + (uint32_t)x * 8;
        uint32_t py = ed_win->content_y + (uint32_t)y * 16;
        fb_render_char_px(px, py, (uint8_t)ch,
                          fb_vga_color(fg), fb_vga_color(bg));
    } else {
        terminal_putentryat(ch, vga_color(fg, bg), (size_t)x, (size_t)y);
    }
}

static void ed_draw_str(int x, int y, const char *s, uint8_t fg, uint8_t bg) {
    for (int i = 0; s[i] && x + i < ed_scr_cols; i++)
        ed_putchar_at(x + i, y, s[i], fg, bg);
}

static void ed_fill_row(int y, uint8_t fg, uint8_t bg) {
    for (int x = 0; x < ed_scr_cols; x++)
        ed_putchar_at(x, y, ' ', fg, bg);
}

/* ------------------------------------------------------------------ */
/*  Line buffer management                                             */
/* ------------------------------------------------------------------ */

static char *line_alloc(int cap) {
    char *p = (char *)kmalloc((size_t)cap);
    if (p) p[0] = '\0';
    return p;
}

static void line_ensure(int idx, int needed) {
    if (needed < ed_cap[idx]) return;
    int new_cap = ed_cap[idx];
    while (new_cap <= needed) new_cap *= 2;
    char *p = (char *)krealloc(ed_lines[idx], (size_t)new_cap);
    if (p) {
        ed_lines[idx] = p;
        ed_cap[idx] = new_cap;
    }
}

static void ed_free_lines(void) {
    for (int i = 0; i < ed_nlines; i++) {
        if (ed_lines[i]) kfree(ed_lines[i]);
        ed_lines[i] = NULL;
    }
    ed_nlines = 0;
}

/* Insert a new empty line at index, shifting subsequent lines down */
static int ed_insert_line(int idx) {
    if (ed_nlines >= ED_MAX_LINES) return -1;
    for (int i = ed_nlines; i > idx; i--) {
        ed_lines[i] = ed_lines[i - 1];
        ed_len[i]   = ed_len[i - 1];
        ed_cap[i]   = ed_cap[i - 1];
    }
    ed_lines[idx] = line_alloc(ED_INIT_LINE_CAP);
    ed_len[idx]   = 0;
    ed_cap[idx]   = ED_INIT_LINE_CAP;
    ed_nlines++;
    return 0;
}

/* Delete line at index, shifting subsequent lines up */
static void ed_delete_line(int idx) {
    if (idx < 0 || idx >= ed_nlines) return;
    kfree(ed_lines[idx]);
    for (int i = idx; i < ed_nlines - 1; i++) {
        ed_lines[i] = ed_lines[i + 1];
        ed_len[i]   = ed_len[i + 1];
        ed_cap[i]   = ed_cap[i + 1];
    }
    ed_nlines--;
    if (ed_nlines == 0) {
        /* Always keep at least one line */
        ed_lines[0] = line_alloc(ED_INIT_LINE_CAP);
        ed_len[0]   = 0;
        ed_cap[0]   = ED_INIT_LINE_CAP;
        ed_nlines   = 1;
    }
}

/* ------------------------------------------------------------------ */
/*  File I/O                                                           */
/* ------------------------------------------------------------------ */

static void ed_load_file(const char *filename) {
    ed_nlines = 0;

    int32_t ino = vfs_resolve(filename, NULL, NULL);
    if (ino < 0) {
        /* New file — start with one empty line */
        ed_lines[0] = line_alloc(ED_INIT_LINE_CAP);
        ed_len[0]   = 0;
        ed_cap[0]   = ED_INIT_LINE_CAP;
        ed_nlines   = 1;
        return;
    }

    vfs_inode_t *node = vfs_get_inode((uint32_t)ino);
    if (!node || node->type != VFS_TYPE_FILE || node->size == 0) {
        ed_lines[0] = line_alloc(ED_INIT_LINE_CAP);
        ed_len[0]   = 0;
        ed_cap[0]   = ED_INIT_LINE_CAP;
        ed_nlines   = 1;
        return;
    }

    /* Split file data on newlines */
    const char *data = (const char *)node->data;
    uint32_t size = node->size;
    uint32_t start = 0;

    for (uint32_t i = 0; i <= size && ed_nlines < ED_MAX_LINES; i++) {
        if (i == size || data[i] == '\n') {
            int len = (int)(i - start);
            int cap = ED_INIT_LINE_CAP;
            while (cap <= len) cap *= 2;
            ed_lines[ed_nlines] = line_alloc(cap);
            if (ed_lines[ed_nlines] && len > 0)
                memcpy(ed_lines[ed_nlines], data + start, (size_t)len);
            if (ed_lines[ed_nlines])
                ed_lines[ed_nlines][len] = '\0';
            ed_len[ed_nlines] = len;
            ed_cap[ed_nlines] = cap;
            ed_nlines++;
            start = i + 1;
        }
    }

    /* If file ended with \n, we may have an extra empty trailing line.
       That's fine — it matches nano behavior. */
    if (ed_nlines == 0) {
        ed_lines[0] = line_alloc(ED_INIT_LINE_CAP);
        ed_len[0]   = 0;
        ed_cap[0]   = ED_INIT_LINE_CAP;
        ed_nlines   = 1;
    }
}

static int ed_save_file(void) {
    int32_t ino = vfs_resolve(ed_filename, NULL, NULL);
    if (ino < 0) {
        ino = vfs_create_file(ed_filename);
        if (ino < 0) return -1;
    }

    vfs_inode_t *node = vfs_get_inode((uint32_t)ino);
    if (!node || node->type != VFS_TYPE_FILE) return -1;

    /* Calculate total size */
    uint32_t total = 0;
    for (int i = 0; i < ed_nlines; i++) {
        total += (uint32_t)ed_len[i];
        if (i < ed_nlines - 1) total++;  /* newline between lines */
    }

    /* Write line by line */
    uint32_t off = 0;
    for (int i = 0; i < ed_nlines; i++) {
        if (ed_len[i] > 0)
            vfs_write((uint32_t)ino, ed_lines[i], off, (uint32_t)ed_len[i]);
        off += (uint32_t)ed_len[i];
        if (i < ed_nlines - 1) {
            vfs_write((uint32_t)ino, "\n", off, 1);
            off++;
        }
    }

    /* Truncate if file was previously longer */
    node->size = total;
    ed_modified = 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Cursor and scroll management                                       */
/* ------------------------------------------------------------------ */

static void ed_clamp_cx(void) {
    if (ed_cx > ed_len[ed_cy])
        ed_cx = ed_len[ed_cy];
}

static void ed_scroll_to_cursor(void) {
    if (ed_cy < ed_scroll)
        ed_scroll = ed_cy;
    if (ed_cy >= ed_scroll + ed_text_rows)
        ed_scroll = ed_cy - ed_text_rows + 1;
}

/* ------------------------------------------------------------------ */
/*  Screen drawing                                                     */
/* ------------------------------------------------------------------ */

static void ed_draw_title(void) {
    ed_fill_row(0, COL_BAR_FG, COL_BAR_BG);

    char title[128];
    int n = 0;
    const char *prefix = " SpikeEdit: ";
    for (int i = 0; prefix[i]; i++)
        title[n++] = prefix[i];
    for (int i = 0; ed_filename[i] && n < 100; i++)
        title[n++] = ed_filename[i];
    if (ed_modified) {
        const char *mod = " [Modified]";
        for (int i = 0; mod[i] && n < 120; i++)
            title[n++] = mod[i];
    }
    title[n] = '\0';

    ed_draw_str(0, 0, title, COL_BAR_FG, COL_BAR_BG);
}

static void ed_draw_text(void) {
    for (int row = 0; row < ed_text_rows; row++) {
        int file_line = ed_scroll + row;
        int screen_row = row + 1;  /* row 0 is title bar */

        if (file_line >= ed_nlines) {
            /* Past end of file — draw tilde like vi */
            ed_fill_row(screen_row, COL_FG, COL_BG);
            ed_putchar_at(0, screen_row, '~', 8, COL_BG);  /* dark gray tilde */
        } else {
            /* Draw line content */
            int len = ed_len[file_line];
            int col;
            for (col = 0; col < ed_scr_cols; col++) {
                if (col < len)
                    ed_putchar_at(col, screen_row, ed_lines[file_line][col],
                                  COL_FG, COL_BG);
                else
                    ed_putchar_at(col, screen_row, ' ', COL_FG, COL_BG);
            }
        }
    }
}

static void ed_draw_status(void) {
    int y = ed_scr_rows - 2;
    ed_fill_row(y, COL_BAR_FG, COL_BAR_BG);

    /* Left side: status message */
    if (ed_status[0])
        ed_draw_str(1, y, ed_status, COL_BAR_FG, COL_BAR_BG);

    /* Right side: cursor position */
    char pos[32];
    /* Manual int-to-string for line:col display */
    int line_num = ed_cy + 1;
    int col_num = ed_cx + 1;
    int pi = 0;
    const char *lbl = "Ln ";
    for (int i = 0; lbl[i]; i++) pos[pi++] = lbl[i];

    /* Format line number */
    char num[12];
    int ni = 0;
    int tmp = line_num;
    if (tmp == 0) num[ni++] = '0';
    else {
        while (tmp > 0) { num[ni++] = '0' + (tmp % 10); tmp /= 10; }
    }
    for (int i = ni - 1; i >= 0; i--) pos[pi++] = num[i];

    pos[pi++] = ',';
    pos[pi++] = ' ';
    const char *clbl = "Col ";
    for (int i = 0; clbl[i]; i++) pos[pi++] = clbl[i];

    ni = 0;
    tmp = col_num;
    if (tmp == 0) num[ni++] = '0';
    else {
        while (tmp > 0) { num[ni++] = '0' + (tmp % 10); tmp /= 10; }
    }
    for (int i = ni - 1; i >= 0; i--) pos[pi++] = num[i];
    pos[pi] = '\0';

    int pos_x = ed_scr_cols - pi - 1;
    if (pos_x > 0)
        ed_draw_str(pos_x, y, pos, COL_BAR_FG, COL_BAR_BG);
}

static void ed_draw_help(void) {
    int y = ed_scr_rows - 1;
    ed_fill_row(y, COL_HELP_FG, COL_HELP_BG);
    ed_draw_str(1, y, "^S Save  ^X Exit  ^K Cut Line", COL_HELP_FG, COL_HELP_BG);
}

static void ed_draw_cursor(void) {
    int screen_x = ed_cx;
    int screen_y = (ed_cy - ed_scroll) + 1;

    if (ed_use_fb && ed_win) {
        /* Draw underline cursor */
        uint32_t px = ed_win->content_x + (uint32_t)screen_x * 8;
        uint32_t py = ed_win->content_y + (uint32_t)screen_y * 16 + 14;
        fb_fill_rect(px, py, 8, 2, fb_vga_color(COL_FG));
    } else {
        terminal_setcursor((size_t)screen_x, (size_t)screen_y);
    }
}

static void ed_draw_all(void) {
    ed_draw_title();
    ed_draw_text();
    ed_draw_status();
    ed_draw_help();
    ed_draw_cursor();
}

/* ------------------------------------------------------------------ */
/*  Text editing operations                                            */
/* ------------------------------------------------------------------ */

static void ed_insert_char(char c) {
    line_ensure(ed_cy, ed_len[ed_cy] + 1);
    /* Shift chars right */
    for (int i = ed_len[ed_cy]; i > ed_cx; i--)
        ed_lines[ed_cy][i] = ed_lines[ed_cy][i - 1];
    ed_lines[ed_cy][ed_cx] = c;
    ed_len[ed_cy]++;
    ed_lines[ed_cy][ed_len[ed_cy]] = '\0';
    ed_cx++;
    ed_modified = 1;
}

static void ed_insert_newline(void) {
    if (ed_nlines >= ED_MAX_LINES) return;

    /* Split current line at cursor */
    int tail_len = ed_len[ed_cy] - ed_cx;
    ed_insert_line(ed_cy + 1);

    /* Copy tail of current line to new line */
    if (tail_len > 0) {
        line_ensure(ed_cy + 1, tail_len);
        memcpy(ed_lines[ed_cy + 1], ed_lines[ed_cy] + ed_cx, (size_t)tail_len);
        ed_lines[ed_cy + 1][tail_len] = '\0';
        ed_len[ed_cy + 1] = tail_len;
    }

    /* Truncate current line at cursor */
    ed_len[ed_cy] = ed_cx;
    ed_lines[ed_cy][ed_cx] = '\0';

    ed_cy++;
    ed_cx = 0;
    ed_modified = 1;
}

static void ed_backspace(void) {
    if (ed_cx > 0) {
        /* Delete char before cursor */
        for (int i = ed_cx - 1; i < ed_len[ed_cy] - 1; i++)
            ed_lines[ed_cy][i] = ed_lines[ed_cy][i + 1];
        ed_len[ed_cy]--;
        ed_lines[ed_cy][ed_len[ed_cy]] = '\0';
        ed_cx--;
        ed_modified = 1;
    } else if (ed_cy > 0) {
        /* Join with previous line */
        int prev = ed_cy - 1;
        int old_len = ed_len[prev];
        int cur_len = ed_len[ed_cy];

        line_ensure(prev, old_len + cur_len);
        memcpy(ed_lines[prev] + old_len, ed_lines[ed_cy], (size_t)cur_len);
        ed_len[prev] = old_len + cur_len;
        ed_lines[prev][ed_len[prev]] = '\0';

        ed_delete_line(ed_cy);
        ed_cy = prev;
        ed_cx = old_len;
        ed_modified = 1;
    }
}

static void ed_delete(void) {
    if (ed_cx < ed_len[ed_cy]) {
        /* Delete char at cursor */
        for (int i = ed_cx; i < ed_len[ed_cy] - 1; i++)
            ed_lines[ed_cy][i] = ed_lines[ed_cy][i + 1];
        ed_len[ed_cy]--;
        ed_lines[ed_cy][ed_len[ed_cy]] = '\0';
        ed_modified = 1;
    } else if (ed_cy < ed_nlines - 1) {
        /* Join with next line */
        int next = ed_cy + 1;
        int cur_len = ed_len[ed_cy];
        int next_len = ed_len[next];

        line_ensure(ed_cy, cur_len + next_len);
        memcpy(ed_lines[ed_cy] + cur_len, ed_lines[next], (size_t)next_len);
        ed_len[ed_cy] = cur_len + next_len;
        ed_lines[ed_cy][ed_len[ed_cy]] = '\0';

        ed_delete_line(next);
        ed_modified = 1;
    }
}

static void ed_cut_line(void) {
    ed_delete_line(ed_cy);
    if (ed_cy >= ed_nlines)
        ed_cy = ed_nlines - 1;
    ed_clamp_cx();
    ed_modified = 1;
}

/* ------------------------------------------------------------------ */
/*  Exit confirmation prompt                                           */
/* ------------------------------------------------------------------ */

/* Returns: 1 = user wants to exit, 0 = cancel */
static int ed_confirm_exit(void) {
    const char *msg = "Save modified buffer? (Y)es (N)o (C)ancel";
    int y = ed_scr_rows - 2;
    ed_fill_row(y, COL_BAR_FG, COL_BAR_BG);
    ed_draw_str(1, y, msg, COL_BAR_FG, COL_BAR_BG);
    ed_draw_cursor();

    while (1) {
        wm_process_events();
        key_event_t k = keyboard_get_event();
        if (k.type == KEY_CHAR) {
            if (k.ch == 'y' || k.ch == 'Y') {
                if (ed_save_file() == 0) {
                    strcpy(ed_status, "Saved");
                } else {
                    strcpy(ed_status, "Save failed!");
                }
                return 1;
            } else if (k.ch == 'n' || k.ch == 'N') {
                return 1;
            } else if (k.ch == 'c' || k.ch == 'C') {
                ed_status[0] = '\0';
                return 0;
            }
        } else if (k.type == KEY_CTRL_C) {
            ed_status[0] = '\0';
            return 0;
        }
        __asm__ volatile("hlt");
    }
}

/* ------------------------------------------------------------------ */
/*  Main editor loop                                                   */
/* ------------------------------------------------------------------ */

void editor_run(const char *filename) {
    /* Store filename */
    int i;
    for (i = 0; filename[i] && i < ED_FILENAME_MAX - 1; i++)
        ed_filename[i] = filename[i];
    ed_filename[i] = '\0';

    /* Detect display mode */
    ed_use_fb = fb_console_active();
    ed_win = wm_get_shell_window();

    if (ed_use_fb && ed_win) {
        ed_scr_cols = (int)fb_console_get_cols();
        ed_scr_rows = (int)fb_console_get_rows();
    } else {
        ed_scr_cols = 80;
        ed_scr_rows = 25;
    }
    ed_text_rows = ed_scr_rows - 3;  /* title, status, help */

    /* Load file */
    ed_load_file(filename);
    ed_cx = 0;
    ed_cy = 0;
    ed_scroll = 0;
    ed_modified = 0;
    ed_status[0] = '\0';

    /* Clear screen */
    if (ed_use_fb && ed_win) {
        fb_fill_rect(ed_win->content_x, ed_win->content_y,
                     ed_win->content_w, ed_win->content_h,
                     fb_vga_color(COL_BG));
    } else {
        /* VGA: clear manually */
        for (int y = 0; y < ed_scr_rows; y++)
            for (int x = 0; x < ed_scr_cols; x++)
                terminal_putentryat(' ', vga_color(COL_FG, COL_BG),
                                    (size_t)x, (size_t)y);
    }

    ed_draw_all();

    /* Main loop */
    int running = 1;
    while (running) {
        wm_process_events();

        key_event_t k = keyboard_get_event();
        if (k.type == KEY_NONE) {
            __asm__ volatile("hlt");
            continue;
        }

        /* Clear status message on any key */
        ed_status[0] = '\0';

        switch (k.type) {
        case KEY_CHAR:
            ed_insert_char(k.ch);
            break;

        case KEY_ENTER:
            ed_insert_newline();
            break;

        case KEY_BACKSPACE:
            ed_backspace();
            break;

        case KEY_DELETE:
            ed_delete();
            break;

        case KEY_LEFT:
            if (ed_cx > 0) {
                ed_cx--;
            } else if (ed_cy > 0) {
                ed_cy--;
                ed_cx = ed_len[ed_cy];
            }
            break;

        case KEY_RIGHT:
            if (ed_cx < ed_len[ed_cy]) {
                ed_cx++;
            } else if (ed_cy < ed_nlines - 1) {
                ed_cy++;
                ed_cx = 0;
            }
            break;

        case KEY_UP:
            if (ed_cy > 0) {
                ed_cy--;
                ed_clamp_cx();
            }
            break;

        case KEY_DOWN:
            if (ed_cy < ed_nlines - 1) {
                ed_cy++;
                ed_clamp_cx();
            }
            break;

        case KEY_HOME:
            ed_cx = 0;
            break;

        case KEY_END:
            ed_cx = ed_len[ed_cy];
            break;

        case KEY_PAGE_UP:
            ed_cy -= ed_text_rows;
            if (ed_cy < 0) ed_cy = 0;
            ed_clamp_cx();
            break;

        case KEY_PAGE_DOWN:
            ed_cy += ed_text_rows;
            if (ed_cy >= ed_nlines) ed_cy = ed_nlines - 1;
            ed_clamp_cx();
            break;

        case KEY_CTRL_S:
            if (ed_save_file() == 0)
                strcpy(ed_status, "Saved");
            else
                strcpy(ed_status, "Save failed!");
            break;

        case KEY_CTRL_X:
            if (ed_modified) {
                if (ed_confirm_exit())
                    running = 0;
            } else {
                running = 0;
            }
            break;

        case KEY_CTRL_K:
            ed_cut_line();
            break;

        case KEY_CTRL_C:
            /* Exit without saving */
            running = 0;
            break;

        default:
            break;
        }

        ed_scroll_to_cursor();
        ed_draw_all();
    }

    /* Clean up */
    ed_free_lines();

    /* Restore console */
    terminal_clear();
}
