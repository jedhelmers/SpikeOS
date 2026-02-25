#ifndef _SURFACE_H
#define _SURFACE_H

#include <stdint.h>

typedef struct {
    uint32_t *pixels;   /* heap-allocated pixel data (XRGB8888) */
    uint32_t  width;    /* pixels */
    uint32_t  height;   /* pixels */
    uint32_t  pitch;    /* bytes per row (= width * 4) */
} surface_t;

/* Allocate a new surface (returns NULL on OOM) */
surface_t *surface_create(uint32_t w, uint32_t h);

/* Free a surface and its pixel buffer */
void surface_destroy(surface_t *s);

/* Fill entire surface with a single color */
void surface_clear(surface_t *s, uint32_t color);

/* Set a single pixel (bounds-checked) */
void surface_putpixel(surface_t *s, uint32_t x, uint32_t y, uint32_t color);

/* Fill a rectangle within the surface */
void surface_fill_rect(surface_t *s, uint32_t x, uint32_t y,
                        uint32_t w, uint32_t h, uint32_t color);

/* Render an 8x16 CP437 glyph at pixel position (px, py) in the surface */
void surface_render_char(surface_t *s, uint32_t px, uint32_t py,
                          uint8_t ch, uint32_t fg, uint32_t bg);

/* Draw a horizontal line within the surface */
void surface_draw_hline(surface_t *s, uint32_t x, uint32_t y,
                         uint32_t w, uint32_t color);

/* Scroll surface contents up by row_h pixels, clear bottom with bg_color */
void surface_scroll_up(surface_t *s, uint32_t row_h, uint32_t bg_color);

/* Blit surface contents to the framebuffer at (dst_x, dst_y) */
void surface_blit_to_fb(surface_t *s, uint32_t dst_x, uint32_t dst_y);

#endif
