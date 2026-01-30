#ifndef _KERNEL_TTY_H
#define _KERNEL_TTY_H

#include <kernel/io.h>
#include <stddef.h>

#define VGA_FG_MASK 0x0F
#define VGA_BG_MASK 0xF0

enum vga_color;

void terminal_initialize(void);
void terminal_putchar(char c);
void terminal_write(const char* data, size_t size);
void terminal_writestring(const char* data);
void terminal_clear(void);
void terminal_setforeground(uint8_t fg);
void terminal_setbackground(uint8_t bg);

#endif