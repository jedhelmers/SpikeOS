#include <kernel/framebuffer.h>
#include <kernel/multiboot.h>
#include <kernel/paging.h>
#include <kernel/io.h>
#include <string.h>

framebuffer_info_t fb_info;

/*
 * FB virtual address: starts at PDE[770] = 0xC0800000.
 * fb_init() maps as many pages as needed, spanning multiple PDEs
 * for large resolutions (e.g. 1920x1080x32 = ~8MB).
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
        if (map_page(virt, phys,
                     PAGE_PRESENT | PAGE_WRITABLE | PAGE_CACHE_DISABLE) != 0) {
            printf("[fb] map_page failed at virt=0x%x\n", virt);
            fb_info.available = 0;
            return;
        }
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

    /*
     * Bochs VBE always uses XRGB8888: pixel = 0x00RRGGBB.
     * The GRUB-reported color positions came from EFI GOP, which may differ.
     * Now that we've reprogrammed Bochs VBE, force the known layout.
     */
    fb_info.red_pos    = 16;  fb_info.red_mask   = 8;
    fb_info.green_pos  =  8;  fb_info.green_mask = 8;
    fb_info.blue_pos   =  0;  fb_info.blue_mask  = 8;
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

void fb_fill_circle(uint32_t cx, uint32_t cy, uint32_t r, uint32_t color) {
    if (!fb_info.available || r == 0) return;
    int ri = (int)r;
    for (int dy = -ri; dy <= ri; dy++) {
        /* Integer sqrt: find largest dx where dx² + dy² <= r² */
        int r2 = ri * ri;
        int dy2 = dy * dy;
        int dx = 0;
        while ((dx + 1) * (dx + 1) + dy2 <= r2) dx++;
        int py = (int)cy + dy;
        if (py < 0) continue;
        int px = (int)cx - dx;
        if (px < 0) px = 0;
        fb_fill_rect((uint32_t)px, (uint32_t)py, (uint32_t)(2 * dx + 1), 1, color);
    }
}

void fb_fill_circle_aa(uint32_t cx, uint32_t cy, uint32_t r,
                        uint32_t color, uint32_t bg_color) {
    if (!fb_info.available || r == 0) return;
    int ri = (int)r;

    /* Pre-extract colour channels for blending */
    uint32_t rmask = (1u << fb_info.red_mask) - 1;
    uint32_t gmask = (1u << fb_info.green_mask) - 1;
    uint32_t bmask = (1u << fb_info.blue_mask) - 1;

    uint32_t cr = (color    >> fb_info.red_pos)   & rmask;
    uint32_t cg = (color    >> fb_info.green_pos) & gmask;
    uint32_t cb = (color    >> fb_info.blue_pos)  & bmask;
    uint32_t br = (bg_color >> fb_info.red_pos)   & rmask;
    uint32_t bg = (bg_color >> fb_info.green_pos) & gmask;
    uint32_t bb = (bg_color >> fb_info.blue_pos)  & bmask;

    /* Radius squared in 8x fixed-point */
    int r2_fp = 64 * ri * ri;

    /* 4x4 subpixel sample offsets within an 8x8 sub-grid per pixel */
    static const int sp[16][2] = {
        {1,1},{3,1},{5,1},{7,1},
        {1,3},{3,3},{5,3},{7,3},
        {1,5},{3,5},{5,5},{7,5},
        {1,7},{3,7},{5,7},{7,7}
    };

    for (int dy = -ri - 1; dy <= ri + 1; dy++) {
        int py = (int)cy + dy;
        if (py < 0 || py >= (int)fb_info.height) continue;

        for (int dx = -ri - 1; dx <= ri + 1; dx++) {
            int px = (int)cx + dx;
            if (px < 0 || px >= (int)fb_info.width) continue;

            /* Count subpixel samples inside the circle */
            int inside = 0;
            for (int s = 0; s < 16; s++) {
                int sx = 8 * dx + sp[s][0] - 4;  /* center at pixel middle */
                int sy = 8 * dy + sp[s][1] - 4;
                if (sx * sx + sy * sy <= r2_fp)
                    inside++;
            }

            if (inside == 0) continue;
            if (inside == 16) {
                fb_putpixel((uint32_t)px, (uint32_t)py, color);
            } else {
                int outside = 16 - inside;
                uint32_t rr = (cr * inside + br * outside + 8) >> 4;
                uint32_t gg = (cg * inside + bg * outside + 8) >> 4;
                uint32_t bbb = (cb * inside + bb * outside + 8) >> 4;
                fb_putpixel((uint32_t)px, (uint32_t)py,
                            fb_pack_color((uint8_t)rr, (uint8_t)gg, (uint8_t)bbb));
            }
        }
    }
}

void fb_draw_hline(uint32_t x, uint32_t y, uint32_t w, uint32_t color) {
    if (!fb_info.available) return;
    if (y >= fb_info.height || x >= fb_info.width) return;
    if (x + w > fb_info.width) w = fb_info.width - x;

    uint32_t bpp = fb_info.bpp / 8;
    volatile uint8_t *row = (volatile uint8_t *)
        (fb_info.virt_addr + y * fb_info.pitch + x * bpp);

    if (fb_info.bpp == 32) {
        volatile uint32_t *p = (volatile uint32_t *)row;
        for (uint32_t i = 0; i < w; i++)
            p[i] = color;
    } else {
        for (uint32_t i = 0; i < w; i++) {
            row[i * 3]     = color & 0xFF;
            row[i * 3 + 1] = (color >> 8) & 0xFF;
            row[i * 3 + 2] = (color >> 16) & 0xFF;
        }
    }
}

void fb_draw_vline(uint32_t x, uint32_t y, uint32_t h, uint32_t color) {
    if (!fb_info.available) return;
    if (x >= fb_info.width || y >= fb_info.height) return;
    if (y + h > fb_info.height) h = fb_info.height - y;

    uint32_t bpp = fb_info.bpp / 8;

    for (uint32_t row = 0; row < h; row++) {
        volatile uint8_t *p = (volatile uint8_t *)
            (fb_info.virt_addr + (y + row) * fb_info.pitch + x * bpp);
        if (fb_info.bpp == 32) {
            *(volatile uint32_t *)p = color;
        } else {
            p[0] = color & 0xFF;
            p[1] = (color >> 8) & 0xFF;
            p[2] = (color >> 16) & 0xFF;
        }
    }
}

void fb_draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    if (!fb_info.available || w == 0 || h == 0) return;
    fb_draw_hline(x, y, w, color);             /* top */
    fb_draw_hline(x, y + h - 1, w, color);     /* bottom */
    fb_draw_vline(x, y, h, color);             /* left */
    fb_draw_vline(x + w - 1, y, h, color);     /* right */
}

void fb_blit(uint32_t dst_x, uint32_t dst_y, const uint32_t *src,
             uint32_t src_pitch, uint32_t w, uint32_t h) {
    if (!fb_info.available) return;
    if (dst_x >= fb_info.width || dst_y >= fb_info.height) return;
    if (dst_x + w > fb_info.width) w = fb_info.width - dst_x;
    if (dst_y + h > fb_info.height) h = fb_info.height - dst_y;

    uint32_t bpp = fb_info.bpp / 8;

    for (uint32_t row = 0; row < h; row++) {
        volatile uint8_t *dst = (volatile uint8_t *)
            (fb_info.virt_addr + (dst_y + row) * fb_info.pitch + dst_x * bpp);
        const uint8_t *srow = (const uint8_t *)src + row * src_pitch;
        memcpy((void *)dst, srow, w * bpp);
    }
}

void fb_blit_masked(uint32_t dst_x, uint32_t dst_y, const uint32_t *src,
                    const uint8_t *mask, uint32_t src_pitch, uint32_t w, uint32_t h) {
    if (!fb_info.available) return;

    uint32_t bpp = fb_info.bpp / 8;

    for (uint32_t row = 0; row < h; row++) {
        uint32_t sy = dst_y + row;
        if (sy >= fb_info.height) break;

        const uint32_t *srow = (const uint32_t *)((const uint8_t *)src + row * src_pitch);
        const uint8_t *mrow = mask + row * w;

        for (uint32_t col = 0; col < w; col++) {
            uint32_t sx = dst_x + col;
            if (sx >= fb_info.width) break;
            if (!mrow[col]) continue;  /* transparent */

            volatile uint8_t *p = (volatile uint8_t *)
                (fb_info.virt_addr + sy * fb_info.pitch + sx * bpp);
            if (fb_info.bpp == 32) {
                *(volatile uint32_t *)p = srow[col];
            } else {
                uint32_t c = srow[col];
                p[0] = c & 0xFF;
                p[1] = (c >> 8) & 0xFF;
                p[2] = (c >> 16) & 0xFF;
            }
        }
    }
}
