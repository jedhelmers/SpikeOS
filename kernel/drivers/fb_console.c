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

/* Character buffer for content redraw after window moves */
typedef struct {
    uint8_t ch;
    uint8_t fg;
    uint8_t bg;
} fb_cell_t;

#define MAX_COLS 128   /* 1024/8 */
#define MAX_ROWS  48   /* 768/16 */

static fb_cell_t char_buf[MAX_ROWS][MAX_COLS];

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

/* Scroll the window up by one character row (FONT_H pixels) */
static void fb_scroll(void) {
    if (!fb_active || !bound_window) return;

    uint32_t win_px = bound_window->content_x;
    uint32_t win_py = bound_window->content_y;
    uint32_t win_pw = bound_window->content_w;
    uint32_t win_ph = bound_window->content_h;

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
}

void fb_console_setcolor(uint8_t fg, uint8_t bg) {
    fg_idx = fg & 0x0F;
    bg_idx = bg & 0x0F;
    update_colors();
}

void fb_console_setcursor(size_t x, size_t y) {
    cx = (uint32_t)x;
    cy = (uint32_t)y;
}

int fb_console_active(void) {
    return fb_active;
}
