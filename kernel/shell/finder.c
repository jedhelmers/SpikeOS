#include <kernel/finder.h>
#include <kernel/window.h>
#include <kernel/vfs.h>
#include <kernel/surface.h>
#include <kernel/framebuffer.h>
#include <kernel/keyboard.h>
#include <kernel/mouse.h>
#include <kernel/timer.h>
#include <kernel/process.h>
#include <kernel/hal.h>
#include <kernel/gui_editor.h>
#include <kernel/heap.h>
#include <kernel/spikefs.h>
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/*  Constants                                                          */
/* ------------------------------------------------------------------ */

#define FONT_W 8
#define FONT_H 16

#define MAX_FINDERS        2
#define FINDER_MAX_ENTRIES 256
#define FINDER_MAX_HIST    32

/* Layout */
#define PATHBAR_H    20
#define COLHDR_H     18
#define ROW_H        18
#define STATUSBAR_H  18
#define SIDEBAR_W    120
#define SCROLLBAR_W  12
#define ICON_SZ      10   /* small file/folder icon */

#define DBLCLICK_TICKS 40  /* 400ms at 100Hz */

/* ------------------------------------------------------------------ */
/*  Colors                                                             */
/* ------------------------------------------------------------------ */

static uint32_t c_pathbar_bg;
static uint32_t c_pathbar_fg;
static uint32_t c_path_sep;
static uint32_t c_sidebar_bg;
static uint32_t c_sidebar_fg;
static uint32_t c_sidebar_sel;
static uint32_t c_colhdr_bg;
static uint32_t c_colhdr_fg;
static uint32_t c_row_even;
static uint32_t c_row_odd;
static uint32_t c_row_sel;
static uint32_t c_row_fg;
static uint32_t c_row_sel_fg;
static uint32_t c_folder_icon;
static uint32_t c_file_icon;
static uint32_t c_size_fg;
static uint32_t c_status_bg;
static uint32_t c_status_fg;
static uint32_t c_scroll_track;
static uint32_t c_scroll_thumb;
static uint32_t c_divider;
static uint32_t c_rename_bg;
static uint32_t c_rename_fg;
static uint32_t c_rename_cursor;

static int colors_inited = 0;

static void init_colors(void) {
    if (colors_inited) return;
    c_pathbar_bg   = fb_pack_color(40, 44, 52);
    c_pathbar_fg   = fb_pack_color(200, 200, 210);
    c_path_sep     = fb_pack_color(120, 120, 130);
    c_sidebar_bg   = fb_pack_color(35, 38, 45);
    c_sidebar_fg   = fb_pack_color(180, 180, 190);
    c_sidebar_sel  = fb_pack_color(50, 90, 160);
    c_colhdr_bg    = fb_pack_color(45, 48, 55);
    c_colhdr_fg    = fb_pack_color(220, 220, 230);
    c_row_even     = fb_pack_color(28, 30, 36);
    c_row_odd      = fb_pack_color(34, 37, 43);
    c_row_sel      = fb_pack_color(40, 80, 160);
    c_row_fg       = fb_pack_color(200, 200, 210);
    c_row_sel_fg   = fb_pack_color(255, 255, 255);
    c_folder_icon  = fb_pack_color(220, 180, 80);
    c_file_icon    = fb_pack_color(100, 140, 200);
    c_size_fg      = fb_pack_color(140, 140, 150);
    c_status_bg    = fb_pack_color(40, 44, 52);
    c_status_fg    = fb_pack_color(160, 160, 170);
    c_scroll_track = fb_pack_color(30, 33, 40);
    c_scroll_thumb = fb_pack_color(80, 85, 95);
    c_divider      = fb_pack_color(55, 58, 65);
    c_rename_bg    = fb_pack_color(50, 55, 70);
    c_rename_fg    = fb_pack_color(255, 255, 255);
    c_rename_cursor = fb_pack_color(200, 200, 220);
    colors_inited  = 1;
}

/* ------------------------------------------------------------------ */
/*  Data structures                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    char     name[60];
    uint32_t inode;
    uint8_t  type;       /* VFS_TYPE_FILE or VFS_TYPE_DIR */
    uint32_t size;
} finder_entry_t;

typedef struct {
    int       active;
    window_t *win;

    /* Current directory */
    char      path[256];
    uint32_t  dir_ino;
    finder_entry_t entries[FINDER_MAX_ENTRIES];
    int       entry_count;

    /* Selection & scroll */
    int       selected;       /* -1 = none */
    int       scroll;         /* first visible row index */
    int       visible_rows;

    /* Navigation history */
    char      history[FINDER_MAX_HIST][256];
    int       hist_count;
    int       hist_pos;

    /* Inline rename */
    int       renaming;
    int       rename_idx;
    char      rename_buf[60];
    int       rename_cursor;
    int       rename_len;

    int       dirty;     /* set by menu actions to signal redraw needed */
    int       quit;
} finder_t;

static finder_t finders[MAX_FINDERS];
static int pending_slot = -1;

/* ------------------------------------------------------------------ */
/*  Sidebar locations                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *label;
    const char *path;
} sidebar_loc_t;

static const sidebar_loc_t sidebar_locs[] = {
    { "/",       "/"                          },
    { "Home",    "/Users/jedhelmers"          },
    { "Desktop", "/Users/jedhelmers/Desktop"  },
};
#define SIDEBAR_LOC_COUNT 3

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

static void render_string(surface_t *s, uint32_t x, uint32_t y,
                           const char *str, uint32_t fg, uint32_t bg) {
    while (*str) {
        if (x + FONT_W > s->width) break;
        surface_render_char(s, x, y, (uint8_t)*str, fg, bg);
        x += FONT_W;
        str++;
    }
}

static void render_string_clipped(surface_t *s, uint32_t x, uint32_t y,
                                    const char *str, uint32_t fg, uint32_t bg,
                                    uint32_t max_w) {
    uint32_t drawn = 0;
    while (*str && drawn + FONT_W <= max_w) {
        if (x + FONT_W > s->width) break;
        surface_render_char(s, x, y, (uint8_t)*str, fg, bg);
        x += FONT_W;
        drawn += FONT_W;
        str++;
    }
}

/* Integer to decimal string — returns pointer to NUL terminator */
static char *uint_to_str(uint32_t val, char *buf) {
    if (val == 0) { *buf++ = '0'; *buf = '\0'; return buf; }
    char tmp[12];
    int len = 0;
    while (val) { tmp[len++] = '0' + (val % 10); val /= 10; }
    for (int i = len - 1; i >= 0; i--) *buf++ = tmp[i];
    *buf = '\0';
    return buf;
}

static char *int_to_str(int val, char *buf) {
    if (val < 0) { *buf++ = '-'; val = -val; }
    return uint_to_str((uint32_t)val, buf);
}

/* Build "dir/name" path. Handles root "/" correctly. */
static void build_path(char *dst, int dst_sz, const char *dir, const char *name) {
    dst[0] = '\0';
    strncpy(dst, dir, dst_sz - 1);
    dst[dst_sz - 1] = '\0';
    int len = strlen(dst);
    if (len > 0 && dst[len - 1] != '/' && len < dst_sz - 1) {
        dst[len++] = '/';
        dst[len] = '\0';
    }
    strncpy(dst + len, name, dst_sz - 1 - len);
    dst[dst_sz - 1] = '\0';
}

/* Set window title to "Finder: <path>" */
static void set_finder_title(finder_t *fm) {
    strcpy(fm->win->title, "Finder: ");
    int plen = strlen(fm->path);
    if (plen > 23) plen = 23;  /* keep title under 32 chars */
    memcpy(fm->win->title + 8, fm->path, plen);
    fm->win->title[8 + plen] = '\0';
}

static void format_size(uint32_t size, char *buf, int buf_sz) {
    (void)buf_sz;
    char *p = buf;
    if (size < 1024) {
        p = uint_to_str(size, p);
        strcpy(p, " B");
    } else if (size < 1024 * 1024) {
        p = uint_to_str(size / 1024, p);
        strcpy(p, " KB");
    } else {
        p = uint_to_str(size / (1024 * 1024), p);
        strcpy(p, " MB");
    }
}

/* ------------------------------------------------------------------ */
/*  Directory loading                                                  */
/* ------------------------------------------------------------------ */

static void finder_load_dir(finder_t *fm) {
    int32_t ino = vfs_resolve(fm->path, NULL, NULL);
    if (ino < 0) {
        /* Path not found — fall back to root */
        strcpy(fm->path, "/");
        ino = vfs_resolve("/", NULL, NULL);
    }
    fm->dir_ino = (uint32_t)ino;

    vfs_inode_t *dir = vfs_get_inode(fm->dir_ino);
    fm->entry_count = 0;

    if (!dir || dir->type != VFS_TYPE_DIR) return;

    vfs_dirent_t *dirents = (vfs_dirent_t *)dir->data;
    uint32_t count = dir->size;

    for (uint32_t i = 0; i < count && fm->entry_count < FINDER_MAX_ENTRIES; i++) {
        /* Skip . and .. */
        if (dirents[i].name[0] == '.' &&
            (dirents[i].name[1] == '\0' ||
             (dirents[i].name[1] == '.' && dirents[i].name[2] == '\0')))
            continue;

        finder_entry_t *e = &fm->entries[fm->entry_count];
        strncpy(e->name, dirents[i].name, 59);
        e->name[59] = '\0';
        e->inode = dirents[i].inode;

        vfs_inode_t *child = vfs_get_inode(e->inode);
        if (child) {
            e->type = child->type;
            e->size = (child->type == VFS_TYPE_FILE) ? child->size : 0;
        } else {
            e->type = VFS_TYPE_FILE;
            e->size = 0;
        }
        fm->entry_count++;
    }

    /* Sort: directories first, then alphabetical within each group */
    for (int i = 1; i < fm->entry_count; i++) {
        finder_entry_t tmp = fm->entries[i];
        int j = i - 1;
        while (j >= 0) {
            int a_dir = (fm->entries[j].type == VFS_TYPE_DIR) ? 0 : 1;
            int b_dir = (tmp.type == VFS_TYPE_DIR) ? 0 : 1;
            if (a_dir > b_dir ||
                (a_dir == b_dir && strcmp(fm->entries[j].name, tmp.name) > 0)) {
                fm->entries[j + 1] = fm->entries[j];
                j--;
            } else {
                break;
            }
        }
        fm->entries[j + 1] = tmp;
    }

    /* Clamp selection and scroll */
    if (fm->entry_count == 0)
        fm->selected = -1;
    else if (fm->selected >= fm->entry_count)
        fm->selected = fm->entry_count - 1;
    if (fm->selected < 0 && fm->entry_count > 0)
        fm->selected = 0;
    fm->scroll = 0;
}

/* ------------------------------------------------------------------ */
/*  Navigation                                                         */
/* ------------------------------------------------------------------ */

static void finder_navigate(finder_t *fm, const char *new_path) {
    /* Push current path to history */
    if (fm->hist_pos < FINDER_MAX_HIST) {
        strcpy(fm->history[fm->hist_pos], fm->path);
        fm->hist_pos++;
        fm->hist_count = fm->hist_pos;
    }

    strncpy(fm->path, new_path, 255);
    fm->path[255] = '\0';
    fm->selected = 0;
    finder_load_dir(fm);
    fm->dirty = 1;

    /* Update window title */
    set_finder_title(fm);
}

static void finder_go_up(finder_t *fm) {
    if (strcmp(fm->path, "/") == 0) return;

    char parent[256];
    strncpy(parent, fm->path, 255);
    parent[255] = '\0';

    /* Trim trailing slash */
    int len = strlen(parent);
    if (len > 1 && parent[len - 1] == '/')
        parent[--len] = '\0';

    /* Find last slash */
    char *last = parent;
    for (char *p = parent; *p; p++)
        if (*p == '/') last = p;

    if (last == parent)
        strcpy(parent, "/");
    else
        *last = '\0';

    finder_navigate(fm, parent);
}

static void finder_go_back(finder_t *fm) {
    if (fm->hist_pos <= 0) return;
    /* Save current before going back */
    if (fm->hist_pos < FINDER_MAX_HIST)
        strcpy(fm->history[fm->hist_pos], fm->path);
    if (fm->hist_pos == fm->hist_count)
        fm->hist_count = fm->hist_pos + 1;

    fm->hist_pos--;
    strncpy(fm->path, fm->history[fm->hist_pos], 255);
    fm->path[255] = '\0';
    fm->selected = 0;
    finder_load_dir(fm);
    fm->dirty = 1;
    set_finder_title(fm);
}

static void finder_go_forward(finder_t *fm) {
    if (fm->hist_pos >= fm->hist_count - 1) return;
    fm->hist_pos++;
    strncpy(fm->path, fm->history[fm->hist_pos], 255);
    fm->path[255] = '\0';
    fm->selected = 0;
    finder_load_dir(fm);
    fm->dirty = 1;
    set_finder_title(fm);
}

static void finder_open_selected(finder_t *fm) {
    if (fm->selected < 0 || fm->selected >= fm->entry_count) return;
    finder_entry_t *e = &fm->entries[fm->selected];

    if (e->type == VFS_TYPE_DIR) {
        char new_path[256];
        build_path(new_path, sizeof(new_path), fm->path, e->name);
        finder_navigate(fm, new_path);
    } else {
        char file_path[256];
        build_path(file_path, sizeof(file_path), fm->path, e->name);
        gui_editor_open(file_path);
    }
}

/* ------------------------------------------------------------------ */
/*  Scroll helpers                                                     */
/* ------------------------------------------------------------------ */

static void finder_compute_visible(finder_t *fm) {
    uint32_t list_h = fm->win->content_h - PATHBAR_H - COLHDR_H - STATUSBAR_H;
    fm->visible_rows = (int)(list_h / ROW_H);
    if (fm->visible_rows < 1) fm->visible_rows = 1;
}

static void finder_ensure_visible(finder_t *fm) {
    if (fm->selected < 0) return;
    if (fm->selected < fm->scroll)
        fm->scroll = fm->selected;
    if (fm->selected >= fm->scroll + fm->visible_rows)
        fm->scroll = fm->selected - fm->visible_rows + 1;
    if (fm->scroll < 0) fm->scroll = 0;
    int max_scroll = fm->entry_count - fm->visible_rows;
    if (max_scroll < 0) max_scroll = 0;
    if (fm->scroll > max_scroll) fm->scroll = max_scroll;
}

/* ------------------------------------------------------------------ */
/*  Drawing                                                            */
/* ------------------------------------------------------------------ */

static void finder_draw(finder_t *fm) {
    surface_t *s = fm->win->surface;
    if (!s) return;

    uint32_t cw = fm->win->content_w;
    uint32_t ch = fm->win->content_h;

    finder_compute_visible(fm);

    /* 1. Path bar */
    surface_fill_rect(s, 0, 0, cw, PATHBAR_H, c_pathbar_bg);
    {
        uint32_t px = 8;
        uint32_t py = (PATHBAR_H - FONT_H) / 2;
        const char *p = fm->path;

        if (*p == '/') {
            surface_render_char(s, px, py, '/', c_pathbar_fg, c_pathbar_bg);
            px += FONT_W;
            p++;
        }

        while (*p) {
            /* Find next '/' */
            const char *seg = p;
            while (*p && *p != '/') p++;

            /* Render " > " separator */
            if (seg != fm->path + 1 && seg != fm->path) {
                render_string(s, px, py, " > ", c_path_sep, c_pathbar_bg);
                px += 3 * FONT_W;
            }

            /* Render segment */
            while (seg < p) {
                if (px + FONT_W > cw) break;
                surface_render_char(s, px, py, (uint8_t)*seg, c_pathbar_fg, c_pathbar_bg);
                px += FONT_W;
                seg++;
            }

            if (*p == '/') p++;
        }
    }

    /* Divider below path bar */
    surface_draw_hline(s, 0, PATHBAR_H - 1, cw, c_divider);

    /* 2. Sidebar */
    uint32_t sb_top = PATHBAR_H;
    uint32_t sb_h = ch - PATHBAR_H - STATUSBAR_H;
    surface_fill_rect(s, 0, sb_top, SIDEBAR_W, sb_h, c_sidebar_bg);

    for (int i = 0; i < SIDEBAR_LOC_COUNT; i++) {
        uint32_t iy = sb_top + 4 + (uint32_t)i * ROW_H;
        int is_current = (strcmp(fm->path, sidebar_locs[i].path) == 0);

        if (is_current) {
            surface_fill_rect(s, 2, iy, SIDEBAR_W - 4, ROW_H, c_sidebar_sel);
            render_string_clipped(s, 8, iy + 1, sidebar_locs[i].label,
                                   c_row_sel_fg, c_sidebar_sel, SIDEBAR_W - 12);
        } else {
            render_string_clipped(s, 8, iy + 1, sidebar_locs[i].label,
                                   c_sidebar_fg, c_sidebar_bg, SIDEBAR_W - 12);
        }
    }

    /* Sidebar divider */
    surface_draw_vline(s, SIDEBAR_W - 1, sb_top, sb_h, c_divider);

    /* 3. Column headers */
    uint32_t list_x = SIDEBAR_W;
    uint32_t list_w = cw - SIDEBAR_W;
    uint32_t hdr_y = PATHBAR_H;

    surface_fill_rect(s, list_x, hdr_y, list_w, COLHDR_H, c_colhdr_bg);

    uint32_t name_col_x = list_x + ICON_SZ + 8;
    uint32_t size_col_x = list_x + list_w - SCROLLBAR_W - 16 * FONT_W;
    uint32_t type_col_x = list_x + list_w - SCROLLBAR_W - 7 * FONT_W;

    render_string(s, name_col_x, hdr_y + 1, "Name", c_colhdr_fg, c_colhdr_bg);
    render_string(s, size_col_x, hdr_y + 1, "Size", c_colhdr_fg, c_colhdr_bg);
    render_string(s, type_col_x, hdr_y + 1, "Type", c_colhdr_fg, c_colhdr_bg);

    surface_draw_hline(s, list_x, hdr_y + COLHDR_H - 1, list_w, c_divider);

    /* 4. File list */
    uint32_t list_top = PATHBAR_H + COLHDR_H;
    uint32_t list_h = ch - PATHBAR_H - COLHDR_H - STATUSBAR_H;
    uint32_t list_content_w = list_w - SCROLLBAR_W;

    /* Clear list area */
    surface_fill_rect(s, list_x, list_top, list_content_w, list_h, c_row_even);

    for (int i = 0; i < fm->visible_rows; i++) {
        int idx = fm->scroll + i;
        if (idx >= fm->entry_count) break;

        finder_entry_t *e = &fm->entries[idx];
        uint32_t ry = list_top + (uint32_t)i * ROW_H;
        int is_sel = (idx == fm->selected);

        /* Row background */
        uint32_t row_bg = is_sel ? c_row_sel :
                          ((idx & 1) ? c_row_odd : c_row_even);
        uint32_t row_fg = is_sel ? c_row_sel_fg : c_row_fg;

        surface_fill_rect(s, list_x, ry, list_content_w, ROW_H, row_bg);

        /* Icon (small colored rectangle) */
        uint32_t icon_x = list_x + 4;
        uint32_t icon_y = ry + (ROW_H - ICON_SZ) / 2;
        uint32_t icon_color = (e->type == VFS_TYPE_DIR) ? c_folder_icon : c_file_icon;
        surface_fill_rect(s, icon_x, icon_y, ICON_SZ, ICON_SZ, icon_color);

        /* Name */
        if (fm->renaming && idx == fm->rename_idx) {
            /* Render rename input box */
            uint32_t rn_x = name_col_x;
            uint32_t rn_w = size_col_x - name_col_x - 4;
            surface_fill_rect(s, rn_x, ry + 1, rn_w, ROW_H - 2, c_rename_bg);

            /* Render rename text */
            render_string_clipped(s, rn_x + 2, ry + 1,
                                   fm->rename_buf, c_rename_fg, c_rename_bg,
                                   rn_w - 4);

            /* Cursor */
            uint32_t cur_x = rn_x + 2 + (uint32_t)fm->rename_cursor * FONT_W;
            if (cur_x < rn_x + rn_w - 2)
                surface_draw_vline(s, cur_x, ry + 2, ROW_H - 4, c_rename_cursor);
        } else {
            uint32_t name_max_w = size_col_x - name_col_x - 4;
            render_string_clipped(s, name_col_x, ry + 1, e->name,
                                   row_fg, row_bg, name_max_w);
        }

        /* Size (files only) */
        if (e->type == VFS_TYPE_FILE) {
            char size_buf[16];
            format_size(e->size, size_buf, sizeof(size_buf));
            uint32_t sfg = is_sel ? c_row_sel_fg : c_size_fg;
            render_string_clipped(s, size_col_x, ry + 1, size_buf,
                                   sfg, row_bg, type_col_x - size_col_x - 4);
        }

        /* Type */
        const char *type_str = (e->type == VFS_TYPE_DIR) ? "Folder" : "File";
        uint32_t tfg = is_sel ? c_row_sel_fg : c_size_fg;
        render_string_clipped(s, type_col_x, ry + 1, type_str,
                               tfg, row_bg, list_content_w - (type_col_x - list_x) - 4);
    }

    /* 5. Scrollbar */
    uint32_t sb_x = list_x + list_content_w;
    surface_fill_rect(s, sb_x, list_top, SCROLLBAR_W, list_h, c_scroll_track);

    if (fm->entry_count > fm->visible_rows) {
        uint32_t track_h = list_h;
        uint32_t thumb_h = (uint32_t)fm->visible_rows * track_h /
                           (uint32_t)fm->entry_count;
        if (thumb_h < 12) thumb_h = 12;
        uint32_t thumb_y = list_top +
                           (uint32_t)fm->scroll * (track_h - thumb_h) /
                           (uint32_t)(fm->entry_count - fm->visible_rows);
        surface_fill_rect(s, sb_x + 2, thumb_y, SCROLLBAR_W - 4, thumb_h,
                           c_scroll_thumb);
    }

    /* 6. Status bar */
    uint32_t stat_y = ch - STATUSBAR_H;
    surface_fill_rect(s, 0, stat_y, cw, STATUSBAR_H, c_status_bg);
    surface_draw_hline(s, 0, stat_y, cw, c_divider);
    {
        char status[64];
        char *sp = status;
        sp = int_to_str(fm->entry_count, sp);
        if (fm->selected >= 0 && fm->selected < fm->entry_count) {
            strcpy(sp, " items  |  "); sp += 11;
            strncpy(sp, fm->entries[fm->selected].name,
                    sizeof(status) - (sp - status) - 1);
            status[sizeof(status) - 1] = '\0';
        } else {
            strcpy(sp, " items");
        }
        render_string_clipped(s, 8, stat_y + 1, status,
                               c_status_fg, c_status_bg, cw - 16);
    }
}

/* ------------------------------------------------------------------ */
/*  Draw + blit helper                                                 */
/* ------------------------------------------------------------------ */

static void finder_draw_and_blit(finder_t *fm) {
    finder_draw(fm);
    /* Use compositor (not direct blit) so overlays like context menus
       and dropdown menus render on top of the Finder content. */
    wm_redraw_all();
}

/* ------------------------------------------------------------------ */
/*  Repaint callback (called by compositor)                            */
/* ------------------------------------------------------------------ */

static void finder_repaint_cb(window_t *win) {
    for (int i = 0; i < MAX_FINDERS; i++) {
        if (finders[i].active && finders[i].win == win) {
            finder_draw(&finders[i]);
            return;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  File operations                                                    */
/* ------------------------------------------------------------------ */

static void finder_new_folder(finder_t *fm) {
    char new_path[256];
    char base_name[] = "New Folder";

    build_path(new_path, sizeof(new_path), fm->path, base_name);

    /* If already exists, append a number */
    int32_t existing = vfs_resolve(new_path, NULL, NULL);
    if (existing >= 0) {
        for (int n = 2; n <= 99; n++) {
            char numbered[64];
            strcpy(numbered, "New Folder ");
            uint_to_str((uint32_t)n, numbered + 11);
            build_path(new_path, sizeof(new_path), fm->path, numbered);
            if (vfs_resolve(new_path, NULL, NULL) < 0) break;
        }
    }

    vfs_mkdir(new_path);
    finder_load_dir(fm);
    fm->dirty = 1;
}

static void finder_new_file(finder_t *fm) {
    char new_path[256];

    build_path(new_path, sizeof(new_path), fm->path, "untitled");

    int32_t existing = vfs_resolve(new_path, NULL, NULL);
    if (existing >= 0) {
        for (int n = 2; n <= 99; n++) {
            char numbered[64];
            strcpy(numbered, "untitled");
            uint_to_str((uint32_t)n, numbered + 8);
            build_path(new_path, sizeof(new_path), fm->path, numbered);
            if (vfs_resolve(new_path, NULL, NULL) < 0) break;
        }
    }

    vfs_create_file(new_path);
    finder_load_dir(fm);
    fm->dirty = 1;
}

static void finder_delete_selected(finder_t *fm) {
    if (fm->selected < 0 || fm->selected >= fm->entry_count) return;
    finder_entry_t *e = &fm->entries[fm->selected];

    char full_path[256];
    build_path(full_path, sizeof(full_path), fm->path, e->name);

    if (e->type == VFS_TYPE_DIR)
        vfs_remove_recursive(full_path);
    else
        vfs_remove(full_path);

    finder_load_dir(fm);
    fm->dirty = 1;
}

/* ------------------------------------------------------------------ */
/*  Inline rename                                                      */
/* ------------------------------------------------------------------ */

static void finder_start_rename(finder_t *fm) {
    if (fm->selected < 0 || fm->selected >= fm->entry_count) return;
    fm->renaming = 1;
    fm->rename_idx = fm->selected;
    strncpy(fm->rename_buf, fm->entries[fm->selected].name, 59);
    fm->rename_buf[59] = '\0';
    fm->rename_len = strlen(fm->rename_buf);
    fm->rename_cursor = fm->rename_len;
    fm->dirty = 1;
}

static void finder_commit_rename(finder_t *fm) {
    if (!fm->renaming) return;
    fm->renaming = 0;

    if (fm->rename_len == 0) return;
    if (strcmp(fm->rename_buf, fm->entries[fm->rename_idx].name) == 0) return;

    char old_path[256], new_path_buf[256];
    build_path(old_path, sizeof(old_path), fm->path,
               fm->entries[fm->rename_idx].name);
    build_path(new_path_buf, sizeof(new_path_buf), fm->path, fm->rename_buf);

    vfs_rename(old_path, new_path_buf);
    finder_load_dir(fm);
    fm->dirty = 1;
}

static void finder_cancel_rename(finder_t *fm) {
    fm->renaming = 0;
}

/* ------------------------------------------------------------------ */
/*  Menu action callbacks                                              */
/* ------------------------------------------------------------------ */

static void action_new_folder(void *ctx)  { finder_new_folder((finder_t *)ctx); }
static void action_new_file(void *ctx)    { finder_new_file((finder_t *)ctx); }
static void action_open(void *ctx)        { finder_open_selected((finder_t *)ctx); }
static void action_delete(void *ctx)      { finder_delete_selected((finder_t *)ctx); }
static void action_rename(void *ctx)      { finder_start_rename((finder_t *)ctx); }
static void action_go_back(void *ctx)     { finder_go_back((finder_t *)ctx); }
static void action_go_forward(void *ctx)  { finder_go_forward((finder_t *)ctx); }
static void action_go_up(void *ctx)       { finder_go_up((finder_t *)ctx); }

static void action_go_home(void *ctx) {
    finder_navigate((finder_t *)ctx, "/");
}

static void action_go_desktop(void *ctx) {
    finder_navigate((finder_t *)ctx, "/Users/jedhelmers/Desktop");
}

/* Forward declaration for hit-testing (defined below) */
static int finder_row_at(finder_t *fm, int32_t mx, int32_t my);

/* ------------------------------------------------------------------ */
/*  Right-click context menu builder                                   */
/* ------------------------------------------------------------------ */

static int finder_build_ctx_menu(window_t *win, int32_t mx, int32_t my) {
    /* Find the finder instance that owns this window */
    finder_t *fm = NULL;
    for (int i = 0; i < MAX_FINDERS; i++) {
        if (finders[i].active && finders[i].win == win) {
            fm = &finders[i];
            break;
        }
    }
    if (!fm) return 0;

    int row = finder_row_at(fm, mx, my);

    if (row >= 0) {
        /* Right-clicked on an entry — select it and show entry actions */
        fm->selected = row;
        fm->dirty = 1;
        wm_menu_add_item(&win->ctx_menu, "Open",   action_open,   fm);
        wm_menu_add_item(&win->ctx_menu, "Rename", action_rename, fm);
        wm_menu_add_item(&win->ctx_menu, "Delete", action_delete, fm);
    } else {
        /* Right-clicked on empty space — show creation actions */
        wm_menu_add_item(&win->ctx_menu, "New Folder", action_new_folder, fm);
        wm_menu_add_item(&win->ctx_menu, "New File",   action_new_file,   fm);
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/*  Mouse hit-testing                                                  */
/* ------------------------------------------------------------------ */

/* Returns the file list row index at screen coordinates, or -1 */
static int finder_row_at(finder_t *fm, int32_t mx, int32_t my) {
    int32_t cx = (int32_t)fm->win->content_x;
    int32_t cy = (int32_t)fm->win->content_y;

    /* Must be in file list area (right of sidebar, below headers, above status) */
    int32_t list_left = cx + SIDEBAR_W;
    int32_t list_top = cy + PATHBAR_H + COLHDR_H;
    int32_t list_right = cx + (int32_t)fm->win->content_w - SCROLLBAR_W;
    int32_t list_bottom = cy + (int32_t)fm->win->content_h - STATUSBAR_H;

    if (mx < list_left || mx >= list_right) return -1;
    if (my < list_top || my >= list_bottom) return -1;

    int row = (int)(my - list_top) / ROW_H + fm->scroll;
    if (row < 0 || row >= fm->entry_count) return -1;
    return row;
}

/* Returns the sidebar location index at screen coordinates, or -1 */
static int finder_sidebar_at(finder_t *fm, int32_t mx, int32_t my) {
    int32_t cx = (int32_t)fm->win->content_x;
    int32_t cy = (int32_t)fm->win->content_y;

    if (mx < cx || mx >= cx + SIDEBAR_W) return -1;
    int32_t sb_top = cy + PATHBAR_H + 4;
    int32_t rel_y = my - sb_top;
    if (rel_y < 0) return -1;

    int idx = rel_y / ROW_H;
    if (idx < 0 || idx >= SIDEBAR_LOC_COUNT) return -1;
    return idx;
}

/* Returns the path bar segment index at screen coordinates, or -1.
   Also fills seg_path with the path up to that segment. */
static int finder_pathbar_at(finder_t *fm, int32_t mx, int32_t my,
                              char *seg_path, int seg_path_sz) {
    int32_t cx = (int32_t)fm->win->content_x;
    int32_t cy = (int32_t)fm->win->content_y;

    if (my < cy || my >= cy + PATHBAR_H) return -1;
    if (mx < cx || mx >= cx + (int32_t)fm->win->content_w) return -1;

    /* Walk through path segments, tracking pixel positions */
    uint32_t px = 8;
    const char *p = fm->path;
    int seg_idx = 0;

    if (*p == '/') {
        /* Root segment "/" */
        if ((int32_t)(cx + px) <= mx && mx < (int32_t)(cx + px + FONT_W)) {
            strncpy(seg_path, "/", seg_path_sz);
            return 0;
        }
        px += FONT_W;
        p++;
        seg_idx = 1;
    }

    char accumulated[256];
    strcpy(accumulated, "/");

    while (*p) {
        const char *seg_start = p;
        while (*p && *p != '/') p++;
        int seg_len = (int)(p - seg_start);

        /* Account for " > " separator */
        if (seg_idx > 1)
            px += 3 * FONT_W;

        uint32_t seg_px_start = px;
        px += (uint32_t)seg_len * FONT_W;

        /* Build accumulated path */
        if (strlen(accumulated) > 1) {
            int alen = strlen(accumulated);
            accumulated[alen] = '/';
            accumulated[alen + 1] = '\0';
        }
        int alen = strlen(accumulated);
        if (alen + seg_len < 255) {
            memcpy(accumulated + alen, seg_start, seg_len);
            accumulated[alen + seg_len] = '\0';
        }

        if ((int32_t)(cx + seg_px_start) <= mx && mx < (int32_t)(cx + px)) {
            strncpy(seg_path, accumulated, seg_path_sz);
            return seg_idx;
        }

        if (*p == '/') p++;
        seg_idx++;
    }

    return -1;
}

/* ------------------------------------------------------------------ */
/*  Finder thread                                                      */
/* ------------------------------------------------------------------ */

static void finder_thread(void) {
    int slot = pending_slot;
    pending_slot = -1;

    if (slot < 0 || slot >= MAX_FINDERS) {
        proc_kill(current_process->pid);
        for (;;) hal_halt();
    }

    finder_t *fm = &finders[slot];

    init_colors();

    /* Create window (700x500, centered) */
    uint32_t win_w = 700, win_h = 500;
    if (win_w > fb_info.width - 40) win_w = fb_info.width - 40;
    if (win_h > fb_info.height - 80) win_h = fb_info.height - 80;
    int32_t win_x = ((int32_t)fb_info.width - (int32_t)win_w) / 2;
    int32_t win_y = ((int32_t)fb_info.height - (int32_t)win_h) / 2 - 20;
    if (win_y < 22) win_y = 22;  /* below deskbar */

    char title[64];
    strcpy(title, "Finder: ");
    strncpy(title + 8, fm->path, sizeof(title) - 9);
    title[sizeof(title) - 1] = '\0';

    fm->win = wm_create_window(win_x, win_y, win_w, win_h, title);
    if (!fm->win) {
        fm->active = 0;
        proc_kill(current_process->pid);
        for (;;) hal_halt();
    }

    fm->win->repaint = finder_repaint_cb;
    fm->win->build_ctx_menu = finder_build_ctx_menu;

    /* Menus */
    wm_menu_t *file_menu = wm_window_add_menu(fm->win, "File");
    wm_menu_add_item(file_menu, "New Folder", action_new_folder, fm);
    wm_menu_add_item(file_menu, "New File", action_new_file, fm);
    wm_menu_add_item(file_menu, "Open", action_open, fm);
    wm_menu_add_item(file_menu, "Rename", action_rename, fm);
    wm_menu_add_item(file_menu, "Delete", action_delete, fm);

    wm_menu_t *go_menu = wm_window_add_menu(fm->win, "Go");
    wm_menu_add_item(go_menu, "Back", action_go_back, fm);
    wm_menu_add_item(go_menu, "Forward", action_go_forward, fm);
    wm_menu_add_item(go_menu, "Go Up", action_go_up, fm);
    wm_menu_add_item(go_menu, "Home", action_go_home, fm);
    wm_menu_add_item(go_menu, "Desktop", action_go_desktop, fm);

    /* Load initial directory */
    finder_load_dir(fm);
    finder_compute_visible(fm);

    wm_focus_window(fm->win);
    wm_redraw_all();

    /* Event loop */
    int prev_lmb = 0;
    uint32_t last_click_tick = 0;
    int last_click_row = -1;
    int redraw = 0;
    uint32_t last_sync_tick = 0;
    #define FINDER_SYNC_TICKS 500  /* 5 seconds at 100Hz */

    while (!fm->quit) {
        wm_process_events();

        /* Check close request */
        if (fm->win->flags & WIN_FLAG_CLOSE_REQ) {
            fm->quit = 1;
            break;
        }

        redraw = 0;

        /* Check if menu actions dirtied the state */
        if (fm->dirty) {
            fm->dirty = 0;
            redraw = 1;
            /* Sync mouse tracking so the click that triggered the menu
               action isn't re-interpreted as a new press by our handler */
            prev_lmb = (mouse_get_state().buttons & MOUSE_BTN_LEFT) ? 1 : 0;
        }

        /* Auto write-back: sync dirty VFS to disk periodically */
        {
            uint32_t now = timer_ticks();
            if (vfs_is_dirty() && (now - last_sync_tick) >= FINDER_SYNC_TICKS) {
                spikefs_sync();
                last_sync_tick = now;
            }
        }

        /* Recompute visible rows (window may have been resized) */
        finder_compute_visible(fm);

        /* Scroll wheel */
        if (fm->win->scroll_accum != 0 &&
            (fm->win->flags & WIN_FLAG_FOCUSED)) {
            int32_t dz = fm->win->scroll_accum;
            fm->win->scroll_accum = 0;
            fm->scroll -= dz * 3;
            if (fm->scroll < 0) fm->scroll = 0;
            int max_scroll = fm->entry_count - fm->visible_rows;
            if (max_scroll < 0) max_scroll = 0;
            if (fm->scroll > max_scroll) fm->scroll = max_scroll;
            redraw = 1;
        }

        /* Mouse input */
        mouse_state_t ms = mouse_get_state();
        int cur_lmb = (ms.buttons & MOUSE_BTN_LEFT) ? 1 : 0;
        int32_t mx = ms.x, my = ms.y;

        if (cur_lmb && !prev_lmb && (fm->win->flags & WIN_FLAG_FOCUSED)) {
            /* Left button just pressed */

            /* Cancel rename on click elsewhere */
            if (fm->renaming) {
                int row = finder_row_at(fm, mx, my);
                if (row != fm->rename_idx) {
                    finder_cancel_rename(fm);
                    redraw = 1;
                }
            }

            /* Check sidebar */
            int sb_idx = finder_sidebar_at(fm, mx, my);
            if (sb_idx >= 0) {
                finder_navigate(fm, sidebar_locs[sb_idx].path);
                redraw = 1;
            }

            /* Check path bar */
            if (sb_idx < 0) {
                char seg_path[256];
                int pb = finder_pathbar_at(fm, mx, my, seg_path, sizeof(seg_path));
                if (pb >= 0) {
                    finder_navigate(fm, seg_path);
                    redraw = 1;
                }
            }

            /* Check file list */
            if (sb_idx < 0) {
                int row = finder_row_at(fm, mx, my);
                if (row >= 0) {
                    /* Double-click detection */
                    uint32_t now = timer_ticks();
                    if (row == last_click_row &&
                        (now - last_click_tick) < DBLCLICK_TICKS) {
                        /* Double-click: open */
                        fm->selected = row;
                        finder_open_selected(fm);
                        last_click_row = -1;
                    } else {
                        /* Single click: select */
                        fm->selected = row;
                        last_click_row = row;
                    }
                    last_click_tick = now;
                    redraw = 1;
                }
            }
        }
        prev_lmb = cur_lmb;

        /* Keyboard input (focus-gated) */
        if (!(fm->win->flags & WIN_FLAG_FOCUSED)) {
            if (!redraw)
                hal_halt();
            else
                finder_draw_and_blit(fm);
            continue;
        }

        key_event_t k = keyboard_get_event();

        if (k.type != KEY_NONE) {
            if (fm->renaming) {
                /* Rename mode keyboard handling */
                switch (k.type) {
                case KEY_CHAR:
                    if (fm->rename_len < 58) {
                        /* Insert character at cursor */
                        memmove(fm->rename_buf + fm->rename_cursor + 1,
                                fm->rename_buf + fm->rename_cursor,
                                fm->rename_len - fm->rename_cursor + 1);
                        fm->rename_buf[fm->rename_cursor] = k.ch;
                        fm->rename_cursor++;
                        fm->rename_len++;
                    }
                    break;
                case KEY_BACKSPACE:
                    if (fm->rename_cursor > 0) {
                        memmove(fm->rename_buf + fm->rename_cursor - 1,
                                fm->rename_buf + fm->rename_cursor,
                                fm->rename_len - fm->rename_cursor + 1);
                        fm->rename_cursor--;
                        fm->rename_len--;
                    }
                    break;
                case KEY_DELETE:
                    if (fm->rename_cursor < fm->rename_len) {
                        memmove(fm->rename_buf + fm->rename_cursor,
                                fm->rename_buf + fm->rename_cursor + 1,
                                fm->rename_len - fm->rename_cursor);
                        fm->rename_len--;
                    }
                    break;
                case KEY_LEFT:
                    if (fm->rename_cursor > 0) fm->rename_cursor--;
                    break;
                case KEY_RIGHT:
                    if (fm->rename_cursor < fm->rename_len) fm->rename_cursor++;
                    break;
                case KEY_HOME:
                    fm->rename_cursor = 0;
                    break;
                case KEY_END:
                    fm->rename_cursor = fm->rename_len;
                    break;
                case KEY_ENTER:
                    finder_commit_rename(fm);
                    break;
                case KEY_CTRL_C:
                    finder_cancel_rename(fm);
                    break;
                default:
                    break;
                }
                redraw = 1;
            } else {
                /* Normal mode keyboard handling */
                switch (k.type) {
                case KEY_UP:
                    if (fm->selected > 0) {
                        fm->selected--;
                        finder_ensure_visible(fm);
                    }
                    redraw = 1;
                    break;
                case KEY_DOWN:
                    if (fm->selected < fm->entry_count - 1) {
                        fm->selected++;
                        finder_ensure_visible(fm);
                    }
                    redraw = 1;
                    break;
                case KEY_ENTER:
                    finder_open_selected(fm);
                    redraw = 1;
                    break;
                case KEY_BACKSPACE:
                    finder_go_up(fm);
                    redraw = 1;
                    break;
                case KEY_HOME:
                    fm->selected = 0;
                    finder_ensure_visible(fm);
                    redraw = 1;
                    break;
                case KEY_END:
                    if (fm->entry_count > 0)
                        fm->selected = fm->entry_count - 1;
                    finder_ensure_visible(fm);
                    redraw = 1;
                    break;
                case KEY_PAGE_UP:
                    fm->selected -= fm->visible_rows;
                    if (fm->selected < 0) fm->selected = 0;
                    finder_ensure_visible(fm);
                    redraw = 1;
                    break;
                case KEY_PAGE_DOWN:
                    fm->selected += fm->visible_rows;
                    if (fm->selected >= fm->entry_count)
                        fm->selected = fm->entry_count - 1;
                    if (fm->selected < 0) fm->selected = 0;
                    finder_ensure_visible(fm);
                    redraw = 1;
                    break;
                case KEY_DELETE:
                    finder_delete_selected(fm);
                    redraw = 1;
                    break;
                default:
                    break;
                }
            }
        }

        if (redraw) {
            finder_draw_and_blit(fm);
        } else {
            hal_halt();
        }
    }

    /* Cleanup */
    window_t *w = fm->win;
    fm->win = NULL;
    fm->active = 0;
    wm_destroy_window(w);

    /* Refocus shell */
    window_t *sw = wm_get_shell_window();
    if (sw) {
        wm_focus_window(sw);
        wm_redraw_all();
    }

    proc_kill(current_process->pid);
    for (;;) hal_halt();
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

void finder_open(const char *path) {
    int slot = -1;
    for (int i = 0; i < MAX_FINDERS; i++) {
        if (!finders[i].active) { slot = i; break; }
    }
    if (slot < 0) return;  /* all slots in use */

    finder_t *fm = &finders[slot];
    memset(fm, 0, sizeof(*fm));
    fm->active = 1;
    fm->selected = 0;

    if (path && *path)
        strncpy(fm->path, path, 255);
    else
        strcpy(fm->path, "/");
    fm->path[255] = '\0';

    pending_slot = slot;
    proc_create_kernel_thread(finder_thread);
}
