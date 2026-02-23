#include <kernel/fb_console.h>
#include <kernel/framebuffer.h>
#include <string.h>

/*
 * Framebuffer text console — renders CP437 glyphs (8x16) onto a linear
 * framebuffer. Provides the same interface as VGA text mode: character grid,
 * cursor, 16-color palette, newline/backspace handling, and scrolling.
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

/* Render a single glyph at character grid position (gx, gy) */
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

/* Scroll the framebuffer up by one character row (FONT_H pixels) */
static void fb_scroll(void) {
    if (!fb_active) return;

    uint32_t row_bytes = fb_info.pitch;
    uint32_t scroll_pixels = FONT_H;

    /* Move everything up by FONT_H pixel rows */
    volatile uint8_t *dst = (volatile uint8_t *)fb_info.virt_addr;
    volatile uint8_t *src = dst + scroll_pixels * row_bytes;
    uint32_t move_size = (fb_info.height - scroll_pixels) * row_bytes;

    memmove((void *)dst, (const void *)src, move_size);

    /* Clear the bottom character row */
    uint32_t clear_y = (rows - 1) * FONT_H;
    fb_fill_rect(0, clear_y, fb_info.width, FONT_H, bg_color);
}

void fb_console_init(void) {
    if (!fb_info.available) return;

    cols = fb_info.width / FONT_W;
    rows = fb_info.height / FONT_H;
    cx = 0;
    cy = 0;

    update_colors();
    fb_active = 1;
}

void fb_console_putchar(char c) {
    if (!fb_active) return;

    fb_render_char(cx, cy, (uint8_t)c, fg_color, bg_color);
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
    if (!fb_active) return;

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
                fb_render_char(cx, cy, ' ', fg_color, bg_color);
            }
            break;

        default:
            fb_console_putchar(data[i]);
            break;
        }
    }
}

void fb_console_clear(void) {
    if (!fb_active) return;
    fb_clear(bg_color);
    cx = 0;
    cy = 0;
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
