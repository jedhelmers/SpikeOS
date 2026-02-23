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

#endif
