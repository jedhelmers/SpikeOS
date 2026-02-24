#ifndef _FB_CONSOLE_H
#define _FB_CONSOLE_H

#include <stdint.h>
#include <stddef.h>
#include <kernel/window.h>

/* Initialize framebuffer text console (no-op if framebuffer not available) */
void fb_console_init(void);

/* Bind console to a window (reads position/size from it) */
void fb_console_bind_window(window_t *win);

/* Write a single character at cursor, advance cursor */
void fb_console_putchar(char c);

/* Write buffer (handles \n, \t, \b) */
void fb_console_write(const char *data, size_t size);

/* Clear screen, reset cursor */
void fb_console_clear(void);

/* Repaint all text content from character buffer (after window move) */
void fb_console_repaint(void);

/* Set foreground/background using VGA color indices (0-15) */
void fb_console_setcolor(uint8_t fg, uint8_t bg);

/* Set cursor position (character grid coordinates) */
void fb_console_setcursor(size_t x, size_t y);

/* Returns 1 if framebuffer console is active */
int fb_console_active(void);

/* Direct character rendering at grid position (no cursor/console state needed).
   Used by boot splash before fb_console_init(). */
void fb_render_char(uint32_t gx, uint32_t gy, uint8_t ch,
                    uint32_t fg, uint32_t bg);

/* Render a glyph at arbitrary pixel coordinates (not grid-aligned).
   Used by window manager for title bar text. */
void fb_render_char_px(uint32_t px, uint32_t py, uint8_t ch,
                       uint32_t fg, uint32_t bg);

/* Convert VGA color index (0-15) to packed framebuffer pixel color */
uint32_t fb_vga_color(uint8_t vga_idx);

#endif
