#include <kernel/vga13.h>
#include <kernel/tty.h>
#include <kernel/io.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Mode 13h register tables
 * Values sourced from FreeVGA project / OSDev wiki.
 * ------------------------------------------------------------------------- */

/* Miscellaneous Output Register (write: 0x3C2) */
#define MISC13  0x63

/* Sequencer registers SR0-SR4 (index: 0x3C4, data: 0x3C5) */
static const uint8_t SEQ13[5] = {
    0x03, /* SR0: reset – normal operation          */
    0x01, /* SR1: clocking mode – 8-dot char clock  */
    0x0F, /* SR2: map mask – all planes enabled     */
    0x00, /* SR3: character map select – unused     */
    0x0E  /* SR4: memory mode – chain-4, extended   */
};

/* CRTC registers CR0-CR18 (index: 0x3D4, data: 0x3D5) */
static const uint8_t CRT13[25] = {
    0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F, /* CR0-CR7  */
    0x00, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* CR8-CRF  */
    0x9C, 0x8E, 0x8F, 0x28, 0x40, 0x96, 0xB9, 0xA3, 0xFF /* CR10-CR18 */
};

/* Graphics Controller GR0-GR8 (index: 0x3CE, data: 0x3CF) */
static const uint8_t GFX13[9] = {
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x40, /* GR5: shift-256 mode (256-colour)       */
    0x05, /* GR6: graphics mode, A000-AFFF window   */
    0x0F, /* GR7: colour don't care                 */
    0xFF  /* GR8: bit mask                          */
};

/* Attribute Controller AR0-AR20 (reset via 0x3DA read, then write 0x3C0) */
static const uint8_t ATTR13[21] = {
    /* AR0-AR15: palette map (identity) */
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    0x41, /* AR16: mode control – 256-colour, disable line-graphics */
    0x00, /* AR17: overscan colour                  */
    0x0F, /* AR18: colour plane enable              */
    0x00, /* AR19: horizontal pixel panning         */
    0x00  /* AR20: colour select                    */
};

/* -------------------------------------------------------------------------
 * Mode 3 (80x25 text) restoration tables
 * ------------------------------------------------------------------------- */
#define MISC3   0x67

static const uint8_t SEQ3[5] = {
    0x03, 0x00, 0x03, 0x00, 0x02
};

static const uint8_t CRT3[25] = {
    0x5F, 0x4F, 0x50, 0x82, 0x55, 0x81, 0xBF, 0x1F,
    0x00, 0x4F, 0x0D, 0x0E, 0x00, 0x00, 0x00, 0x00,
    0x9C, 0x8E, 0x8F, 0x28, 0x1F, 0x96, 0xB9, 0xA3, 0xFF
};

static const uint8_t GFX3[9] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x0E, 0x00, 0xFF
};

static const uint8_t ATTR3[21] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07,
    0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
    0x0C, /* AR16: text mode, blinking, line-graphics enable */
    0x00, 0x0F, 0x08, 0x00
};

/* -------------------------------------------------------------------------
 * Generic helpers
 * ------------------------------------------------------------------------- */

static void write_regs(uint8_t misc,
                       const uint8_t *seq,
                       const uint8_t *crt,
                       const uint8_t *gfx,
                       const uint8_t *attr)
{
    /* Miscellaneous Output */
    outb(0x3C2, misc);

    /* Sequencer */
    for (int i = 0; i < 5; i++) {
        outb(0x3C4, (uint8_t)i);
        outb(0x3C5, seq[i]);
    }

    /* CRTC – first unlock registers 0-7 by clearing protect bit in CR11 */
    outb(0x3D4, 0x11);
    outb(0x3D5, (uint8_t)(inb(0x3D5) & ~0x80));
    for (int i = 0; i < 25; i++) {
        outb(0x3D4, (uint8_t)i);
        outb(0x3D5, crt[i]);
    }

    /* Graphics Controller */
    for (int i = 0; i < 9; i++) {
        outb(0x3CE, (uint8_t)i);
        outb(0x3CF, gfx[i]);
    }

    /* Attribute Controller – reset flip-flop first via 0x3DA read */
    inb(0x3DA);
    for (int i = 0; i < 21; i++) {
        outb(0x3C0, (uint8_t)i);
        outb(0x3C0, attr[i]);
    }
    outb(0x3C0, 0x20); /* re-enable display (set PAS bit) */
}

/* -------------------------------------------------------------------------
 * Font save / restore
 *
 * Mode 13h uses chain-4 addressing: every 4th framebuffer byte falls in
 * VGA plane 2, which is exactly where the text-mode character font lives.
 * Pixel writes corrupt the glyph data, so we save plane 2 before entering
 * Mode 13h and restore it before returning to text mode.
 *
 * Font layout: 256 characters × 32-byte slots = 8 192 bytes.
 * ------------------------------------------------------------------------- */

static uint8_t font_backup[8192];

static void font_save(void) {
    /* Set up sequential plane-2 read access via the A000h window */
    outb(0x3C4, 0x04); outb(0x3C5, 0x06); /* SR4: sequential, extended  */
    outb(0x3CE, 0x04); outb(0x3CF, 0x02); /* GR4: read from plane 2     */
    outb(0x3CE, 0x05); outb(0x3CF, 0x00); /* GR5: read mode 0           */
    outb(0x3CE, 0x06); outb(0x3CF, 0x05); /* GR6: A000h window          */

    const uint8_t *p2 = (const uint8_t *)0xA0000;
    for (int i = 0; i < 8192; i++)
        font_backup[i] = p2[i];

    /* Restore text-mode register values before handing control back */
    outb(0x3C4, 0x04); outb(0x3C5, 0x02); /* SR4 */
    outb(0x3CE, 0x04); outb(0x3CF, 0x00); /* GR4 */
    outb(0x3CE, 0x05); outb(0x3CF, 0x10); /* GR5 */
    outb(0x3CE, 0x06); outb(0x3CF, 0x0E); /* GR6 */
}

static void font_restore(void) {
    /* Write exclusively to plane 2 via the A000h window */
    outb(0x3C4, 0x02); outb(0x3C5, 0x04); /* SR2: plane 2 only          */
    outb(0x3C4, 0x04); outb(0x3C5, 0x06); /* SR4: sequential, extended  */
    outb(0x3CE, 0x04); outb(0x3CF, 0x02); /* GR4: read from plane 2     */
    outb(0x3CE, 0x05); outb(0x3CF, 0x00); /* GR5: write mode 0          */
    outb(0x3CE, 0x06); outb(0x3CF, 0x05); /* GR6: A000h window          */

    uint8_t *p2 = (uint8_t *)0xA0000;
    for (int i = 0; i < 8192; i++)
        p2[i] = font_backup[i];

    /* Restore text-mode register values */
    outb(0x3C4, 0x02); outb(0x3C5, 0x03); /* SR2 */
    outb(0x3C4, 0x04); outb(0x3C5, 0x02); /* SR4 */
    outb(0x3CE, 0x04); outb(0x3CF, 0x00); /* GR4 */
    outb(0x3CE, 0x05); outb(0x3CF, 0x10); /* GR5 */
    outb(0x3CE, 0x06); outb(0x3CF, 0x0E); /* GR6 */
}

/* -------------------------------------------------------------------------
 * Public API
 *
 * vga_busy: set while VGA registers are being reprogrammed.
 * terminal_update_cursor() checks this and skips its 0x3D4/0x3D5 writes
 * when set, preventing interleaving from a concurrently scheduled thread.
 * The scheduler is NOT disabled — background threads keep running normally.
 * ------------------------------------------------------------------------- */

volatile int vga_busy = 0;

void vga13_enter(void) {
    vga_busy = 1;
    font_save();   /* must happen before Mode 13h corrupts plane 2 */
    write_regs(MISC13, SEQ13, CRT13, GFX13, ATTR13);
    vga_busy = 0;
}

void vga13_exit(void) {
    vga_busy = 1;
    write_regs(MISC3, SEQ3, CRT3, GFX3, ATTR3);
    font_restore(); /* must happen after text-mode registers are in place */
    vga_busy = 0;
    terminal_initialize(); /* repaint VGA text buffer */
}

/* Mode 13h framebuffer: 320 x 200, one byte per pixel (palette index) */
static uint8_t *const FB = (uint8_t *)0xA0000;

void vga13_putpixel(int x, int y, uint8_t c) {
    if ((unsigned)x < 320 && (unsigned)y < 200)
        FB[y * 320 + x] = c;
}

void vga13_fill_rect(int x, int y, int w, int h, uint8_t c) {
    for (int row = y; row < y + h; row++)
        for (int col = x; col < x + w; col++)
            vga13_putpixel(col, row, c);
}

void vga13_clear(uint8_t c) {
    memset(FB, c, 320 * 200);
}

void vga13_set_palette(uint8_t idx, uint8_t r, uint8_t g, uint8_t b) {
    outb(0x3C8, idx);
    outb(0x3C9, r);
    outb(0x3C9, g);
    outb(0x3C9, b);
}
