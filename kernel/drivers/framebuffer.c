#include <kernel/framebuffer.h>
#include <kernel/multiboot.h>
#include <kernel/paging.h>
#include <kernel/io.h>
#include <string.h>

framebuffer_info_t fb_info;

/*
 * FB virtual address: PDE[770] = 0xC0800000
 * This gives us a 4MB window, enough for up to 1024x1024x32bpp.
 */
#define FB_VIRT_BASE 0xC0800000u

void fb_save_info(struct multiboot_info *mb) {
    memset(&fb_info, 0, sizeof(fb_info));

    if (!mb) return;

    /*
     * Multiboot flag bit 12 indicates framebuffer info is available.
     * GRUB sets this when it provides a linear framebuffer (via VBE or GOP).
     */
    if (!(mb->flags & MB_FLAG_FRAMEBUFFER)) return;

    /* Only accept linear RGB framebuffers (type 1) */
    if (mb->framebuffer_type != 1) return;

    /* Truncate 64-bit address to 32-bit (fine for QEMU/Bochs) */
    fb_info.phys_addr  = (uint32_t)mb->framebuffer_addr;
    fb_info.pitch      = mb->framebuffer_pitch;
    fb_info.width      = mb->framebuffer_width;
    fb_info.height     = mb->framebuffer_height;
    fb_info.bpp        = mb->framebuffer_bpp;
    fb_info.red_pos    = mb->fb_red_pos;
    fb_info.red_mask   = mb->fb_red_mask;
    fb_info.green_pos  = mb->fb_green_pos;
    fb_info.green_mask = mb->fb_green_mask;
    fb_info.blue_pos   = mb->fb_blue_pos;
    fb_info.blue_mask  = mb->fb_blue_mask;
    fb_info.available  = 1;
}

void fb_init(void) {
    if (!fb_info.available) return;

    /* Calculate total framebuffer size in bytes */
    uint32_t fb_size = fb_info.pitch * fb_info.height;

    /* Map framebuffer pages into kernel VA at FB_VIRT_BASE */
    uint32_t phys = fb_info.phys_addr & ~0xFFFu;  /* page-align */
    uint32_t virt = FB_VIRT_BASE;
    uint32_t mapped = 0;

    while (mapped < fb_size) {
        map_page(virt, phys,
                 PAGE_PRESENT | PAGE_WRITABLE | PAGE_CACHE_DISABLE);
        virt += PAGE_SIZE;
        phys += PAGE_SIZE;
        mapped += PAGE_SIZE;
    }

    /* Account for sub-page offset (if phys_addr wasn't page-aligned) */
    fb_info.virt_addr = FB_VIRT_BASE + (fb_info.phys_addr & 0xFFFu);
}

void fb_enable(void) {
    if (!fb_info.available) return;

    /*
     * Re-enable Bochs VBE linear framebuffer mode.
     * terminal_initialize() / vga_set_mode3() disabled VBE to run the boot
     * splash in VGA text mode.  Now that the splash is done, switch back to
     * the linear framebuffer so we can render pixels.
     *
     * Bochs VBE registers: index port 0x01CE, data port 0x01CF.
     */
    outw(0x01CE, 0x04);                  /* VBE_DISPI_INDEX_ENABLE */
    outw(0x01CF, 0x00);                  /* disable first (required for mode change) */

    outw(0x01CE, 0x01);                  /* VBE_DISPI_INDEX_XRES */
    outw(0x01CF, (uint16_t)fb_info.width);
    outw(0x01CE, 0x02);                  /* VBE_DISPI_INDEX_YRES */
    outw(0x01CF, (uint16_t)fb_info.height);
    outw(0x01CE, 0x03);                  /* VBE_DISPI_INDEX_BPP */
    outw(0x01CF, (uint16_t)fb_info.bpp);

    outw(0x01CE, 0x04);                  /* VBE_DISPI_INDEX_ENABLE */
    outw(0x01CF, 0x0041);                /* VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED */
}

void fb_putpixel(uint32_t x, uint32_t y, uint32_t color) {
    if (!fb_info.available) return;
    if (x >= fb_info.width || y >= fb_info.height) return;

    uint32_t offset = y * fb_info.pitch + x * (fb_info.bpp / 8);
    volatile uint8_t *pixel = (volatile uint8_t *)(fb_info.virt_addr + offset);

    if (fb_info.bpp == 32) {
        *((volatile uint32_t *)pixel) = color;
    } else if (fb_info.bpp == 24) {
        pixel[0] = color & 0xFF;
        pixel[1] = (color >> 8) & 0xFF;
        pixel[2] = (color >> 16) & 0xFF;
    }
}

void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    if (!fb_info.available) return;

    /* Clip to screen bounds */
    if (x >= fb_info.width || y >= fb_info.height) return;
    if (x + w > fb_info.width) w = fb_info.width - x;
    if (y + h > fb_info.height) h = fb_info.height - y;

    uint32_t bytes_per_pixel = fb_info.bpp / 8;

    for (uint32_t row = y; row < y + h; row++) {
        volatile uint8_t *rowp = (volatile uint8_t *)
            (fb_info.virt_addr + row * fb_info.pitch + x * bytes_per_pixel);

        if (fb_info.bpp == 32) {
            volatile uint32_t *p = (volatile uint32_t *)rowp;
            for (uint32_t col = 0; col < w; col++)
                p[col] = color;
        } else {
            for (uint32_t col = 0; col < w; col++) {
                rowp[col * 3]     = color & 0xFF;
                rowp[col * 3 + 1] = (color >> 8) & 0xFF;
                rowp[col * 3 + 2] = (color >> 16) & 0xFF;
            }
        }
    }
}

void fb_clear(uint32_t color) {
    fb_fill_rect(0, 0, fb_info.width, fb_info.height, color);
}

uint32_t fb_pack_color(uint8_t r, uint8_t g, uint8_t b) {
    uint32_t color = 0;
    color |= ((uint32_t)r & ((1u << fb_info.red_mask) - 1))   << fb_info.red_pos;
    color |= ((uint32_t)g & ((1u << fb_info.green_mask) - 1)) << fb_info.green_pos;
    color |= ((uint32_t)b & ((1u << fb_info.blue_mask) - 1))  << fb_info.blue_pos;
    return color;
}
