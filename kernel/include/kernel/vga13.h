#ifndef _VGA13_H
#define _VGA13_H

#include <stdint.h>

/* Switch the VGA adapter to Mode 13h (320x200, 256 colours).
   Framebuffer is at physical 0xA0000, which is identity-mapped. */
void vga13_enter(void);

/* Restore VGA text mode (mode 3, 80x25) and repaint the terminal buffer. */
void vga13_exit(void);

/* Fill the entire 320x200 framebuffer with colour index c. */
void vga13_clear(uint8_t c);

/* Draw a filled rectangle. Coordinates are clamped to screen bounds. */
void vga13_fill_rect(int x, int y, int w, int h, uint8_t color);

/* Set a single pixel. No-op if out of bounds. */
void vga13_putpixel(int x, int y, uint8_t color);

/* Program one DAC palette entry. r/g/b are 6-bit (0-63). */
void vga13_set_palette(uint8_t idx, uint8_t r, uint8_t g, uint8_t b);

/* Non-zero while VGA registers are being reprogrammed during a mode switch.
   Code that touches shared VGA ports (e.g. 0x3D4/0x3D5) should check this
   and skip its writes rather than racing with the mode-switch sequence. */
extern volatile int vga_busy;

#endif
