#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "string.h"

#include <kernel/tty.h>
#include <kernel/vga13.h>

#include "vga.h"

static const size_t VGA_WIDTH  =  80;
static const size_t VGA_HEIGHT =  25;
static const size_t TAB = 4;
static uint16_t* const VGA_MEMORY =  (uint16_t*) 0xB8000;

static size_t terminal_row;
static size_t terminal_column;
static uint8_t terminal_color;
static uint16_t* terminal_buffer = (uint16_t*)VGA_MEMORY;


static void terminal_update_cursor(void) {
    if (vga_busy) return;   /* VGA mid-switch; skip to avoid port 0x3D4/5 race */

    size_t pos = terminal_row * VGA_WIDTH + terminal_column;

    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
}

void terminal_initialize(void) {
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
    terminal_initialize();
}

void terminal_scroll(void) {
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
    terminal_column = 0;
    terminal_row++;

    terminal_update_cursor();
}

void terminal_putchar(char c) {
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