#include <kernel/fb_console.h>
#include <kernel/framebuffer.h>
#include <kernel/window.h>
#include <string.h>

/*
 * Framebuffer text console — renders CP437 glyphs (8x16) onto a linear
 * framebuffer. Provides the same interface as VGA text mode: character grid,
 * cursor, 16-color palette, newline/backspace handling, and scrolling.
 *
 * The console binds to a window_t and reads position/size from it,
 * enabling drag-to-move via the window manager.
 */

/* Embedded 8x16 CP437 font (same one used by VGA text mode) */
#include "../arch/i386/vga_font.h"

#define FONT_W 8
#define FONT_H 16

static int fb_active = 0;     /* 1 after fb_console_init succeeds */
static uint32_t cols, rows;   /* character grid dimensions */
static uint32_t cx, cy;       /* cursor position (character coords) */
static uint8_t fg_idx = 7;    /* foreground VGA color index (light gray) */
static uint8_t bg_idx = 0;    /* background VGA color index (black) */
static uint32_t fg_color;     /* packed foreground pixel color */
static uint32_t bg_color;     /* packed background pixel color */

/* Bound window — position/size read from here */
static window_t *bound_window = NULL;

/* Forward declarations for cursor helpers */
static void draw_cursor(void);
static void erase_cursor(void);

/* Character buffer for content redraw after window moves */
typedef struct {
    uint8_t ch;
    uint8_t fg;
    uint8_t bg;
} fb_cell_t;

#define MAX_COLS 128   /* 1024/8 */
#define MAX_ROWS  48   /* 768/16 */

static fb_cell_t char_buf[MAX_ROWS][MAX_COLS];

/* ------------------------------------------------------------------ */
/*  Scrollback ring buffer                                             */
/* ------------------------------------------------------------------ */

#define SB_LINES 200

static fb_cell_t sb_ring[SB_LINES][MAX_COLS];
static uint32_t sb_head  = 0;   /* next write slot in ring */
static uint32_t sb_count = 0;   /* lines stored (capped at SB_LINES) */
static int      sb_offset = 0;  /* view offset: 0 = live, >0 = scrolled back */

/* Saved screen snapshot when entering scrollback mode */
static fb_cell_t saved_screen[MAX_ROWS][MAX_COLS];
static uint32_t saved_cx, saved_cy;
static int sb_saved = 0;

/* VGA 16-color palette → RGB (standard CGA/VGA colors) */
static const uint8_t vga_palette[16][3] = {
    {  0,   0,   0},   /* 0  black */
    {  0,   0, 170},   /* 1  blue */
    {  0, 170,   0},   /* 2  green */
    {  0, 170, 170},   /* 3  cyan */
    {170,   0,   0},   /* 4  red */
    {170,   0, 170},   /* 5  magenta */
    {170,  85,   0},   /* 6  brown */
    {170, 170, 170},   /* 7  light gray */
    { 85,  85,  85},   /* 8  dark gray */
    { 85,  85, 255},   /* 9  light blue */
    { 85, 255,  85},   /* 10 light green */
    { 85, 255, 255},   /* 11 light cyan */
    {255,  85,  85},   /* 12 light red */
    {255,  85, 255},   /* 13 light magenta */
    {255, 255,  85},   /* 14 yellow */
    {255, 255, 255},   /* 15 white */
};

uint32_t fb_vga_color(uint8_t idx) {
    if (idx > 15) idx = 7;
    return fb_pack_color(vga_palette[idx][0],
                         vga_palette[idx][1],
                         vga_palette[idx][2]);
}

static void update_colors(void) {
    fg_color = fb_vga_color(fg_idx);
    bg_color = fb_vga_color(bg_idx);
}

/* Render a single glyph at character grid position (gx, gy).
   Used by boot splash with absolute grid coords. */
void fb_render_char(uint32_t gx, uint32_t gy, uint8_t ch,
                    uint32_t fg, uint32_t bg) {
    uint32_t px = gx * FONT_W;
    uint32_t py = gy * FONT_H;
    const uint8_t *glyph = &vga_font_8x16[ch * FONT_H];

    for (uint32_t row = 0; row < FONT_H; row++) {
        uint8_t bits = glyph[row];
        for (uint32_t col = 0; col < FONT_W; col++) {
            uint32_t color = (bits & (0x80 >> col)) ? fg : bg;
            fb_putpixel(px + col, py + row, color);
        }
    }
}

/* Render a single glyph at arbitrary pixel position (not grid-aligned).
   Used by window manager for title bar text and by console rendering. */
void fb_render_char_px(uint32_t px, uint32_t py, uint8_t ch,
                       uint32_t fg, uint32_t bg) {
    const uint8_t *glyph = &vga_font_8x16[ch * FONT_H];

    for (uint32_t row = 0; row < FONT_H; row++) {
        uint8_t bits = glyph[row];
        for (uint32_t col = 0; col < FONT_W; col++) {
            uint32_t color = (bits & (0x80 >> col)) ? fg : bg;
            fb_putpixel(px + col, py + row, color);
        }
    }
}

/* Restore saved screen and exit scrollback mode */
static void fb_snap_to_bottom(void) {
    if (sb_offset > 0 && sb_saved) {
        memcpy(char_buf, saved_screen, sizeof(char_buf));
        cx = saved_cx;
        cy = saved_cy;
        sb_offset = 0;
        sb_saved = 0;
        fb_console_repaint();
        draw_cursor();
    }
}

/* Scroll the window up by one character row (FONT_H pixels) */
static void fb_scroll(void) {
    if (!fb_active || !bound_window) return;

    /* If scrolled back, snap to live view first */
    fb_snap_to_bottom();

    uint32_t win_px = bound_window->content_x;
    uint32_t win_py = bound_window->content_y;
    uint32_t win_pw = bound_window->content_w;
    uint32_t win_ph = bound_window->content_h;

    /* Save the top row into scrollback ring before it's lost */
    memcpy(sb_ring[sb_head], char_buf[0], MAX_COLS * sizeof(fb_cell_t));
    sb_head = (sb_head + 1) % SB_LINES;
    if (sb_count < SB_LINES) sb_count++;

    /* Scroll character buffer */
    memmove(&char_buf[0], &char_buf[1],
            (rows - 1) * MAX_COLS * sizeof(fb_cell_t));
    memset(&char_buf[rows - 1], 0, MAX_COLS * sizeof(fb_cell_t));

    /* Move each pixel row within the window up by FONT_H pixels */
    uint32_t bpp = fb_info.bpp / 8;
    uint32_t win_row_bytes = win_pw * bpp;

    for (uint32_t py = win_py + FONT_H; py < win_py + win_ph; py++) {
        volatile uint8_t *dst = (volatile uint8_t *)
            (fb_info.virt_addr + (py - FONT_H) * fb_info.pitch + win_px * bpp);
        volatile uint8_t *src = (volatile uint8_t *)
            (fb_info.virt_addr + py * fb_info.pitch + win_px * bpp);
        memmove((void *)dst, (const void *)src, win_row_bytes);
    }

    /* Clear the bottom character row of the window */
    uint32_t clear_y = win_py + (rows - 1) * FONT_H;
    fb_fill_rect(win_px, clear_y, win_pw, FONT_H, bg_color);
}

void fb_console_init(void) {
    if (!fb_info.available) return;
    update_colors();
    fb_active = 1;
}

void fb_console_bind_window(window_t *win) {
    bound_window = win;
    cols = win->content_w / FONT_W;
    rows = win->content_h / FONT_H;
    if (cols > MAX_COLS) cols = MAX_COLS;
    if (rows > MAX_ROWS) rows = MAX_ROWS;
    cx = 0;
    cy = 0;
    memset(char_buf, 0, sizeof(char_buf));
    update_colors();
}

void fb_console_putchar(char c) {
    if (!fb_active || !bound_window) return;

    /* Record in character buffer */
    char_buf[cy][cx].ch = (uint8_t)c;
    char_buf[cy][cx].fg = fg_idx;
    char_buf[cy][cx].bg = bg_idx;

    fb_render_char_px(bound_window->content_x + cx * FONT_W,
                      bound_window->content_y + cy * FONT_H,
                      (uint8_t)c, fg_color, bg_color);
    cx++;

    if (cx >= cols) {
        cx = 0;
        cy++;
    }

    if (cy >= rows) {
        fb_scroll();
        cy = rows - 1;
    }
}

void fb_console_write(const char *data, size_t size) {
    if (!fb_active || !bound_window) return;

    fb_snap_to_bottom();
    erase_cursor();

    for (size_t i = 0; i < size; i++) {
        switch (data[i]) {
        case '\n':
            cx = 0;
            cy++;
            if (cy >= rows) {
                fb_scroll();
                cy = rows - 1;
            }
            break;

        case '\t': {
            uint32_t tab = 4 - (cx % 4);
            for (uint32_t t = 0; t < tab; t++)
                fb_console_putchar(' ');
            break;
        }

        case '\b':
            if (cx > 0) {
                cx--;
                char_buf[cy][cx].ch = ' ';
                char_buf[cy][cx].fg = fg_idx;
                char_buf[cy][cx].bg = bg_idx;
                fb_render_char_px(bound_window->content_x + cx * FONT_W,
                                  bound_window->content_y + cy * FONT_H,
                                  ' ', fg_color, bg_color);
            }
            break;

        default:
            fb_console_putchar(data[i]);
            break;
        }
    }

    draw_cursor();
}

void fb_console_repaint(void) {
    if (!fb_active || !bound_window) return;

    uint32_t win_px = bound_window->content_x;
    uint32_t win_py = bound_window->content_y;
    uint32_t win_pw = bound_window->content_w;
    uint32_t win_ph = bound_window->content_h;

    /* Clear content area */
    fb_fill_rect(win_px, win_py, win_pw, win_ph, bg_color);

    /* Repaint from character buffer */
    for (uint32_t r = 0; r < rows; r++) {
        for (uint32_t c = 0; c < cols; c++) {
            fb_cell_t cell = char_buf[r][c];
            if (cell.ch == 0) continue;
            uint32_t cell_fg = fb_vga_color(cell.fg);
            uint32_t cell_bg = fb_vga_color(cell.bg);
            fb_render_char_px(win_px + c * FONT_W, win_py + r * FONT_H,
                              cell.ch, cell_fg, cell_bg);
        }
    }
}

void fb_console_clear(void) {
    if (!fb_active || !bound_window) return;

    /* Redraw desktop and window chrome */
    wm_draw_desktop();
    wm_draw_chrome(bound_window);

    /* Clear content area */
    fb_fill_rect(bound_window->content_x, bound_window->content_y,
                 bound_window->content_w, bound_window->content_h, bg_color);
    cx = 0;
    cy = 0;
    memset(char_buf, 0, sizeof(char_buf));

    /* Reset scrollback */
    sb_head = 0;
    sb_count = 0;
    sb_offset = 0;
    sb_saved = 0;
}

void fb_console_setcolor(uint8_t fg, uint8_t bg) {
    fg_idx = fg & 0x0F;
    bg_idx = bg & 0x0F;
    update_colors();
}

/* ------------------------------------------------------------------ */
/*  Visible cursor                                                     */
/* ------------------------------------------------------------------ */

static int cursor_visible = 0;

/* Draw an underline cursor at the current (cx, cy) position */
static void draw_cursor(void) {
    if (!fb_active || !bound_window) return;
    uint32_t px = bound_window->content_x + cx * FONT_W;
    uint32_t py = bound_window->content_y + cy * FONT_H + (FONT_H - 2);
    fb_fill_rect(px, py, FONT_W, 2, fg_color);
    cursor_visible = 1;
}

/* Erase the cursor by restoring the character at (cx, cy) */
static void erase_cursor(void) {
    if (!cursor_visible || !fb_active || !bound_window) return;
    fb_cell_t cell = char_buf[cy][cx];
    uint32_t cell_fg = (cell.ch != 0) ? fb_vga_color(cell.fg) : fg_color;
    uint32_t cell_bg = (cell.ch != 0) ? fb_vga_color(cell.bg) : bg_color;
    uint8_t ch = (cell.ch != 0) ? cell.ch : ' ';
    fb_render_char_px(bound_window->content_x + cx * FONT_W,
                      bound_window->content_y + cy * FONT_H,
                      ch, cell_fg, cell_bg);
    cursor_visible = 0;
}

void fb_console_setcursor(size_t x, size_t y) {
    erase_cursor();
    cx = (uint32_t)x;
    cy = (uint32_t)y;
    draw_cursor();
}

void fb_console_update_cursor(void) {
    if (!fb_active) return;
    draw_cursor();
}

/* ------------------------------------------------------------------ */
/*  Scrollback navigation                                              */
/* ------------------------------------------------------------------ */

/* Redraw screen from scrollback + saved screen snapshot */
static void fb_redraw_scrollback(void) {
    if (!fb_active || !bound_window) return;

    uint32_t win_px = bound_window->content_x;
    uint32_t win_py = bound_window->content_y;
    uint32_t win_pw = bound_window->content_w;
    uint32_t win_ph = bound_window->content_h;

    /* Clear content area */
    fb_fill_rect(win_px, win_py, win_pw, win_ph, bg_color);

    for (int y = 0; y < (int)rows; y++) {
        /* Virtual line index: 0 = oldest scrollback line */
        int vline = (int)sb_count - sb_offset + y;

        fb_cell_t *src_row = NULL;

        if (vline < 0) {
            /* Past beginning of history — blank */
            continue;
        } else if (vline < (int)sb_count) {
            /* From scrollback ring */
            int idx = ((int)sb_head - (int)sb_count + vline
                       + (int)SB_LINES) % (int)SB_LINES;
            src_row = sb_ring[idx];
        } else {
            /* From saved screen */
            int sy = vline - (int)sb_count;
            if (sy < (int)rows)
                src_row = saved_screen[sy];
        }

        if (!src_row) continue;

        for (uint32_t c = 0; c < cols; c++) {
            if (src_row[c].ch == 0) continue;
            uint32_t cell_fg = fb_vga_color(src_row[c].fg);
            uint32_t cell_bg = fb_vga_color(src_row[c].bg);
            fb_render_char_px(win_px + c * FONT_W, win_py + (uint32_t)y * FONT_H,
                              src_row[c].ch, cell_fg, cell_bg);
        }
    }
}

void fb_console_page_up(void) {
    if (!fb_active || !bound_window) return;
    if (sb_count == 0) return;

    /* Save current screen on first scroll-back */
    if (sb_offset == 0) {
        memcpy(saved_screen, char_buf, sizeof(saved_screen));
        saved_cx = cx;
        saved_cy = cy;
        sb_saved = 1;
        erase_cursor();
    }

    sb_offset += (int)rows;
    if (sb_offset > (int)sb_count) sb_offset = (int)sb_count;

    fb_redraw_scrollback();
}

void fb_console_page_down(void) {
    if (!fb_active || !bound_window) return;
    if (sb_offset == 0) return;

    sb_offset -= (int)rows;
    if (sb_offset <= 0) {
        /* Snap back to live view */
        sb_offset = 0;
        if (sb_saved) {
            memcpy(char_buf, saved_screen, sizeof(char_buf));
            cx = saved_cx;
            cy = saved_cy;
            sb_saved = 0;
        }
        fb_console_repaint();
        draw_cursor();
        return;
    }

    fb_redraw_scrollback();
}

int fb_console_active(void) {
    return fb_active;
}
