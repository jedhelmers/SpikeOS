#ifndef _FRAMEBUFFER_H
#define _FRAMEBUFFER_H

#include <stdint.h>

struct multiboot_info;

typedef struct {
    uint32_t phys_addr;     /* physical base address */
    uint32_t virt_addr;     /* kernel VA after mapping */
    uint32_t pitch;         /* bytes per scanline */
    uint32_t width;         /* pixels */
    uint32_t height;        /* pixels */
    uint8_t  bpp;           /* bits per pixel (expected 32) */
    uint8_t  red_pos, red_mask;
    uint8_t  green_pos, green_mask;
    uint8_t  blue_pos, blue_mask;
    int      available;     /* 1 if GRUB provided framebuffer info */
} framebuffer_info_t;

extern framebuffer_info_t fb_info;

/* Save framebuffer info from multiboot (call early, before paging switch) */
void fb_save_info(struct multiboot_info *mb);

/* Map framebuffer into kernel VA space (call after paging_init + heap_init) */
void fb_init(void);

/* Re-enable VBE linear framebuffer mode (call after boot splash VGA text mode) */
void fb_enable(void);

/* Pixel operations */
void fb_putpixel(uint32_t x, uint32_t y, uint32_t color);
void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void fb_clear(uint32_t color);

/* Pack R/G/B into native pixel format */
uint32_t fb_pack_color(uint8_t r, uint8_t g, uint8_t b);

/* Drawing primitives */
void fb_draw_hline(uint32_t x, uint32_t y, uint32_t w, uint32_t color);
void fb_draw_vline(uint32_t x, uint32_t y, uint32_t h, uint32_t color);
void fb_draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);

/* Blit rectangular pixel buffer to framebuffer (32bpp source, src_pitch in bytes) */
void fb_blit(uint32_t dst_x, uint32_t dst_y, const uint32_t *src,
             uint32_t src_pitch, uint32_t w, uint32_t h);

/* Blit with transparency mask (mask: 1=opaque, 0=transparent, one byte per pixel) */
void fb_blit_masked(uint32_t dst_x, uint32_t dst_y, const uint32_t *src,
                    const uint8_t *mask, uint32_t src_pitch, uint32_t w, uint32_t h);

#endif
