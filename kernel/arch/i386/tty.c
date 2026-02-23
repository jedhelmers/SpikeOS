#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "string.h"

#include <kernel/tty.h>
#include <kernel/vga13.h>
#include <kernel/io.h>
#include <kernel/fb_console.h>

#include "vga.h"

/* Console backend: 0 = VGA text mode (default), 1 = framebuffer */
static int use_fb = 0;

static const size_t VGA_WIDTH  =  80;
static const size_t VGA_HEIGHT =  25;
static const size_t TAB = 4;
static uint16_t* const VGA_MEMORY =  (uint16_t*) 0xB8000;

static size_t terminal_row;
static size_t terminal_column;
static uint8_t terminal_color;
static uint16_t* terminal_buffer = (uint16_t*)VGA_MEMORY;

/* Scrollback ring buffer */
#define SCROLLBACK_LINES 200
static uint16_t scrollback[SCROLLBACK_LINES][80];
static size_t sb_head  = 0;   /* next write slot in ring */
static size_t sb_count = 0;   /* lines stored (capped at SCROLLBACK_LINES) */
static int    sb_offset = 0;  /* view offset: 0 = bottom, >0 = scrolled back */

/* Saved screen snapshot when entering scrollback mode */
static uint16_t saved_screen[25 * 80];
static int sb_saved = 0;


static void terminal_update_cursor(void) {
    if (vga_busy) return;   /* VGA mid-switch; skip to avoid port 0x3D4/5 race */

    size_t pos = terminal_row * VGA_WIDTH + terminal_column;

    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
}

/*
 * vga_set_mode3 -- Force VGA into 80x25 text mode (mode 3).
 *
 * Under UEFI, OVMF sets the display to a GOP framebuffer via Bochs VBE.
 * The legacy VGA registers are left in an undefined state. This function
 * disables VBE and fully reprograms all VGA registers for standard text mode,
 * then reloads the 8x16 font into plane 2 (OVMF's framebuffer writes may
 * have overwritten it).
 *
 * Under BIOS, this is harmless — the VGA is already in mode 3 and we just
 * reprogram it to the same state.
 */
static void vga_set_mode3(void) {
    /* 1. Disable Bochs VBE (QEMU/Bochs) */
    outw(0x01CE, 0x04);  /* VBE_DISPI_INDEX_ENABLE */
    outw(0x01CF, 0x00);  /* VBE_DISPI_DISABLED     */

    /* 2. Miscellaneous Output: 25MHz clock, RAM enable, I/O at 0x3Dx */
    outb(0x3C2, 0x67);

    /* 3. Sequencer */
    outb(0x3C4, 0x00); outb(0x3C5, 0x03);  /* Reset: normal */
    outb(0x3C4, 0x01); outb(0x3C5, 0x00);  /* Clocking: 9-dot */
    outb(0x3C4, 0x02); outb(0x3C5, 0x03);  /* Map Mask: planes 0,1 */
    outb(0x3C4, 0x03); outb(0x3C5, 0x00);  /* Char Map: font 0 */
    outb(0x3C4, 0x04); outb(0x3C5, 0x02);  /* Mem Mode: O/E, no chain4 */

    /* 4. Unlock CRTC, then program all 25 registers */
    outb(0x3D4, 0x11); outb(0x3D5, inb(0x3D5) & 0x7F);

    static const uint8_t crtc[25] = {
        0x5F,0x4F,0x50,0x82,0x55,0x81,0xBF,0x1F,
        0x00,0x4F,0x0D,0x0E,0x00,0x00,0x00,0x00,
        0x9C,0x0E,0x8F,0x28,0x1F,0x96,0xB9,0xA3,
        0xFF
    };
    for (unsigned i = 0; i < 25; i++) {
        outb(0x3D4, i);
        outb(0x3D5, crtc[i]);
    }

    /* 5. Graphics Controller */
    static const uint8_t gc[9] = {
        0x00,0x00,0x00,0x00,0x00,0x10,0x0E,0x00,0xFF
    };
    for (unsigned i = 0; i < 9; i++) {
        outb(0x3CE, i);
        outb(0x3CF, gc[i]);
    }

    /* 6. Attribute Controller */
    static const uint8_t ac[21] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x14,0x07,
        0x38,0x39,0x3A,0x3B,0x3C,0x3D,0x3E,0x3F,
        0x0C,0x00,0x0F,0x08,0x00
    };
    for (unsigned i = 0; i < 21; i++) {
        inb(0x3DA);          /* reset flip-flop */
        outb(0x3C0, i);      /* index */
        outb(0x3C0, ac[i]);  /* data  */
    }
    inb(0x3DA);
    outb(0x3C0, 0x20);  /* re-enable display */

    /* 7. Load 8x16 font into plane 2.
       OVMF's VBE framebuffer writes may have overwritten the font data.
       We copy 256 glyphs from the VGA BIOS ROM at 0xC0000 + 0x???? but
       that ROM may not exist under UEFI. Instead, we poke the VGA sequencer
       to expose plane 2, zero it, then write a minimal built-in font.
       For now, trigger a font reset by toggling the character map select —
       QEMU's VGA emulation keeps the ROM font in a shadow buffer and
       reloads it when we switch to text mode with the correct sequencer
       state (seq reg 2 = 0x04 selects plane 2 for CPU writes). */

    /* Expose plane 2 for CPU access */
    outb(0x3C4, 0x02); outb(0x3C5, 0x04);  /* Map Mask: plane 2 only */
    outb(0x3C4, 0x04); outb(0x3C5, 0x06);  /* Mem Mode: sequential, no O/E */
    outb(0x3CE, 0x04); outb(0x3CF, 0x02);  /* Read Map: plane 2 */
    outb(0x3CE, 0x05); outb(0x3CF, 0x00);  /* Mode: read/write mode 0 */
    outb(0x3CE, 0x06); outb(0x3CF, 0x00);  /* Misc: A000-BFFF window, sequential */

    /* Copy font from VGA ROM shadow (0xC0000 + 0x0FA6E is the standard
       8x16 font location in most VGA BIOSes, but under UEFI/QEMU the
       font lives at a different offset). Use the BIOS font area at
       physical 0xFFA6E as a fallback if 0xC0000 area is valid.

       Safer approach: write a small built-in font covering printable ASCII. */
    volatile uint8_t *plane2 = (volatile uint8_t *)0xA0000;

    /* Include the built-in 8x16 CP437 font */
    #include "vga_font.h"

    for (unsigned ch = 0; ch < 256; ch++) {
        for (unsigned row = 0; row < 16; row++) {
            plane2[ch * 32 + row] = vga_font_8x16[ch * 16 + row];
        }
    }

    /* Restore sequencer/GC to normal text mode */
    outb(0x3C4, 0x02); outb(0x3C5, 0x03);  /* Map Mask: planes 0,1 */
    outb(0x3C4, 0x04); outb(0x3C5, 0x02);  /* Mem Mode: O/E, no chain4 */
    outb(0x3CE, 0x04); outb(0x3CF, 0x00);  /* Read Map: plane 0 */
    outb(0x3CE, 0x05); outb(0x3CF, 0x10);  /* Mode: O/E */
    outb(0x3CE, 0x06); outb(0x3CF, 0x0E);  /* Misc: text, B800-BFFF */
}

void terminal_initialize(void) {
    vga_set_mode3();

    terminal_row = 0;
    terminal_column = 0;

    for (size_t y = 0; y < VGA_HEIGHT; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            terminal_color = vga_entry_color(((y + x) % 15) + 1, VGA_COLOR_BLACK);
            const size_t index = y * VGA_WIDTH + x;
            terminal_buffer[index] = vga_entry(' ', terminal_color);
        }
    }

    terminal_update_cursor();
}

void terminal_clear(void) {
    if (use_fb) {
        fb_console_clear();
        return;
    }
    sb_head = 0;
    sb_count = 0;
    sb_offset = 0;
    sb_saved = 0;
    terminal_initialize();
}

/* Restore saved screen and exit scrollback mode */
static void terminal_snap_to_bottom(void) {
    if (sb_offset > 0 && sb_saved) {
        for (size_t i = 0; i < VGA_HEIGHT * VGA_WIDTH; i++)
            terminal_buffer[i] = saved_screen[i];
        sb_offset = 0;
        sb_saved = 0;
        terminal_update_cursor();
    }
}

void terminal_scroll(void) {
    /* If scrolled back, snap to bottom first */
    terminal_snap_to_bottom();

    /* Save the top row into scrollback ring before it's lost */
    for (size_t x = 0; x < VGA_WIDTH; x++)
        scrollback[sb_head][x] = terminal_buffer[x];
    sb_head = (sb_head + 1) % SCROLLBACK_LINES;
    if (sb_count < SCROLLBACK_LINES) sb_count++;

    // Shift the terminal buffer index
    for (size_t y = 1; y < VGA_HEIGHT; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            terminal_buffer[(y - 1) * VGA_WIDTH + x] = terminal_buffer[y * VGA_WIDTH + x];
        }
    }

    for (size_t x = 0; x < VGA_WIDTH; x++) {
        terminal_buffer[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = vga_entry(' ', terminal_color);
    }

    terminal_update_cursor();
}

void terminal_setcolor(uint8_t color) {
    terminal_color = color;
}

void terminal_putentryat(char c, uint8_t color, size_t x, size_t y) {
    const size_t index = y * VGA_WIDTH + x;
    terminal_buffer[index] = vga_entry(c, color);
}

void terminal_tab() {
    size_t tab_offset = TAB - (terminal_column % TAB);
    for (size_t i = terminal_column; i < tab_offset; i++) {
        terminal_putchar(' ');
    }
}

void terminal_newline() {
    terminal_snap_to_bottom();
    terminal_column = 0;
    terminal_row++;

    if (terminal_row >= VGA_HEIGHT) {
        terminal_scroll();
        terminal_row = VGA_HEIGHT - 1;
    }

    terminal_update_cursor();
}

void terminal_putchar(char c) {
    terminal_snap_to_bottom();
    terminal_putentryat(c, terminal_color, terminal_column, terminal_row);
    terminal_column++;

    // Wrap text
    if (terminal_column >= VGA_WIDTH) {
        terminal_column = 0;
        terminal_row++;
    }

    // Scroll
    if (terminal_row >= VGA_HEIGHT) {
        terminal_scroll();
        terminal_row = VGA_HEIGHT - 1;
    }

    terminal_update_cursor();
}

void terminal_write(const char* data, size_t size) {
    if (use_fb) {
        fb_console_write(data, size);
        return;
    }

    // Iterate through each char in the data
    for (size_t i = 0; i < size; i++) {
        // Handle special chars
        switch (data[i]) {
            case '\n':
                terminal_newline();
                break;
            case '\t':
                terminal_tab();
                break;
            case '\b':
                if (terminal_column > 0) {
                    terminal_column--;
                    terminal_putentryat(' ', terminal_color, terminal_column, terminal_row);
                    terminal_update_cursor();
                }
                break;
            default:
                terminal_putchar(data[i]);
        }
    }
}

void terminal_writestring(const char* data) {
    terminal_write(data, strlen(data));
}

void terminal_setforeground(uint8_t fg) {
    terminal_color = vga_entry_color((enum vga_color)fg, (terminal_color & VGA_BG_MASK) >> 4);
}

void terminal_setbackground(uint8_t bg) {
    terminal_color = vga_entry_color((terminal_color & VGA_FG_MASK), (enum vga_color)bg);
}

void terminal_setcursor(size_t x, size_t y) {
    terminal_column = x;
    terminal_row = y;
    terminal_update_cursor();
}

/* Redraw screen from scrollback + saved screen snapshot */
static void terminal_redraw_scrollback(void) {
    for (int y = 0; y < (int)VGA_HEIGHT; y++) {
        /* virtual line index: 0 = oldest scrollback line */
        int vline = (int)sb_count - sb_offset + y;

        if (vline < 0) {
            /* Past beginning of history — blank line */
            for (size_t x = 0; x < VGA_WIDTH; x++)
                terminal_buffer[y * VGA_WIDTH + x] = vga_entry(' ', terminal_color);
        } else if (vline < (int)sb_count) {
            /* From scrollback ring buffer */
            int idx = ((int)sb_head - (int)sb_count + vline
                       + SCROLLBACK_LINES) % SCROLLBACK_LINES;
            for (size_t x = 0; x < VGA_WIDTH; x++)
                terminal_buffer[y * VGA_WIDTH + x] = scrollback[idx][x];
        } else {
            /* From saved screen snapshot */
            int sy = vline - (int)sb_count;
            if (sy < (int)VGA_HEIGHT) {
                for (size_t x = 0; x < VGA_WIDTH; x++)
                    terminal_buffer[y * VGA_WIDTH + x] = saved_screen[sy * VGA_WIDTH + x];
            }
        }
    }
}

void terminal_page_up(void) {
    if (sb_count == 0) return;

    /* Save current screen on first scroll-back */
    if (sb_offset == 0) {
        for (size_t i = 0; i < VGA_HEIGHT * VGA_WIDTH; i++)
            saved_screen[i] = terminal_buffer[i];
        sb_saved = 1;
    }

    sb_offset += (int)VGA_HEIGHT;
    if (sb_offset > (int)sb_count) sb_offset = (int)sb_count;

    terminal_redraw_scrollback();
}

void terminal_page_down(void) {
    if (sb_offset == 0) return;

    sb_offset -= (int)VGA_HEIGHT;
    if (sb_offset <= 0) {
        /* Snap back to live view */
        sb_offset = 0;
        if (sb_saved) {
            for (size_t i = 0; i < VGA_HEIGHT * VGA_WIDTH; i++)
                terminal_buffer[i] = saved_screen[i];
            sb_saved = 0;
        }
        terminal_update_cursor();
        return;
    }

    terminal_redraw_scrollback();
}

void terminal_switch_to_fb(void) {
    if (fb_console_active())
        use_fb = 1;
}