#include <kernel/surface.h>
#include <kernel/framebuffer.h>
#include <kernel/heap.h>
#include <string.h>

/* Embedded 8x16 CP437 font (same one used by fb_console and VGA text mode) */
#include "../arch/i386/vga_font.h"

#define FONT_W 8
#define FONT_H 16

surface_t *surface_create(uint32_t w, uint32_t h) {
    if (w == 0 || h == 0) return NULL;

    surface_t *s = (surface_t *)kmalloc(sizeof(surface_t));
    if (!s) return NULL;

    s->pixels = (uint32_t *)kmalloc(w * h * 4);
    if (!s->pixels) {
        kfree(s);
        return NULL;
    }

    s->width  = w;
    s->height = h;
    s->pitch  = w * 4;

    /* Clear to black */
    memset(s->pixels, 0, w * h * 4);
    return s;
}

void surface_destroy(surface_t *s) {
    if (!s) return;
    if (s->pixels) kfree(s->pixels);
    kfree(s);
}

void surface_clear(surface_t *s, uint32_t color) {
    if (!s || !s->pixels) return;
    uint32_t total = s->width * s->height;
    if (color == 0) {
        memset(s->pixels, 0, total * 4);
    } else {
        for (uint32_t i = 0; i < total; i++)
            s->pixels[i] = color;
    }
}

void surface_putpixel(surface_t *s, uint32_t x, uint32_t y, uint32_t color) {
    if (!s || !s->pixels) return;
    if (x >= s->width || y >= s->height) return;
    s->pixels[y * s->width + x] = color;
}

void surface_fill_rect(surface_t *s, uint32_t x, uint32_t y,
                        uint32_t w, uint32_t h, uint32_t color) {
    if (!s || !s->pixels) return;
    if (x >= s->width || y >= s->height) return;
    if (x + w > s->width)  w = s->width - x;
    if (y + h > s->height) h = s->height - y;

    for (uint32_t row = y; row < y + h; row++) {
        uint32_t *p = &s->pixels[row * s->width + x];
        for (uint32_t col = 0; col < w; col++)
            p[col] = color;
    }
}

void surface_render_char(surface_t *s, uint32_t px, uint32_t py,
                          uint8_t ch, uint32_t fg, uint32_t bg) {
    if (!s || !s->pixels) return;

    const uint8_t *glyph = &vga_font_8x16[ch * FONT_H];

    for (uint32_t row = 0; row < FONT_H; row++) {
        uint32_t sy = py + row;
        if (sy >= s->height) break;
        uint8_t bits = glyph[row];
        for (uint32_t col = 0; col < FONT_W; col++) {
            uint32_t sx = px + col;
            if (sx >= s->width) break;
            uint32_t color = (bits & (0x80 >> col)) ? fg : bg;
            s->pixels[sy * s->width + sx] = color;
        }
    }
}

void surface_render_char_scaled(surface_t *s, uint32_t px, uint32_t py,
                                 uint8_t ch, uint32_t fg, uint32_t bg,
                                 int scale) {
    if (!s || !s->pixels || scale < 1) return;
    if (scale == 1) {
        surface_render_char(s, px, py, ch, fg, bg);
        return;
    }

    const uint8_t *glyph = &vga_font_8x16[ch * FONT_H];
    uint32_t sc = (uint32_t)scale;

    for (uint32_t row = 0; row < FONT_H; row++) {
        uint8_t bits = glyph[row];
        for (uint32_t col = 0; col < FONT_W; col++) {
            uint32_t color = (bits & (0x80 >> col)) ? fg : bg;
            /* Fill a scale x scale block */
            for (uint32_t sy = 0; sy < sc; sy++) {
                uint32_t dy = py + row * sc + sy;
                if (dy >= s->height) break;
                for (uint32_t sx = 0; sx < sc; sx++) {
                    uint32_t dx = px + col * sc + sx;
                    if (dx >= s->width) break;
                    s->pixels[dy * s->width + dx] = color;
                }
            }
        }
    }
}

void surface_draw_hline(surface_t *s, uint32_t x, uint32_t y,
                         uint32_t w, uint32_t color) {
    if (!s || !s->pixels) return;
    if (y >= s->height || x >= s->width) return;
    if (x + w > s->width) w = s->width - x;

    uint32_t *p = &s->pixels[y * s->width + x];
    for (uint32_t i = 0; i < w; i++)
        p[i] = color;
}

void surface_scroll_up(surface_t *s, uint32_t row_h, uint32_t bg_color) {
    if (!s || !s->pixels || row_h == 0) return;
    if (row_h >= s->height) {
        surface_clear(s, bg_color);
        return;
    }

    uint32_t rows_to_move = s->height - row_h;
    memmove(s->pixels,
            s->pixels + row_h * s->width,
            rows_to_move * s->width * 4);

    /* Clear the bottom row_h rows */
    uint32_t *bottom = s->pixels + rows_to_move * s->width;
    uint32_t count = row_h * s->width;
    for (uint32_t i = 0; i < count; i++)
        bottom[i] = bg_color;
}

void surface_blit_to_fb(surface_t *s, uint32_t dst_x, uint32_t dst_y) {
    if (!s || !s->pixels || !fb_info.available) return;

    uint32_t w = s->width;
    uint32_t h = s->height;

    /* Clip to screen bounds */
    if (dst_x >= fb_info.width || dst_y >= fb_info.height) return;
    if (dst_x + w > fb_info.width)  w = fb_info.width - dst_x;
    if (dst_y + h > fb_info.height) h = fb_info.height - dst_y;

    if (fb_info.bpp == 32) {
        /* Fast path: surface layout matches framebuffer pixel layout */
        for (uint32_t row = 0; row < h; row++) {
            volatile uint8_t *dst = (volatile uint8_t *)
                (fb_info.virt_addr + (dst_y + row) * fb_info.pitch + dst_x * 4);
            uint32_t *src = &s->pixels[row * s->width];
            memcpy((void *)dst, src, w * 4);
        }
    } else {
        /* Slow path: pixel-by-pixel conversion for non-32bpp */
        uint32_t bpp = fb_info.bpp / 8;
        for (uint32_t row = 0; row < h; row++) {
            for (uint32_t col = 0; col < w; col++) {
                uint32_t color = s->pixels[row * s->width + col];
                volatile uint8_t *dst = (volatile uint8_t *)
                    (fb_info.virt_addr + (dst_y + row) * fb_info.pitch
                     + (dst_x + col) * bpp);
                dst[0] = color & 0xFF;
                dst[1] = (color >> 8) & 0xFF;
                dst[2] = (color >> 16) & 0xFF;
            }
        }
    }
}
