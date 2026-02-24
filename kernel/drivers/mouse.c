#include <kernel/mouse.h>
#include <kernel/framebuffer.h>
#include <kernel/event.h>
#include <kernel/isr.h>
#include <kernel/pic.h>
#include <kernel/io.h>
#include <kernel/hal.h>

/* 8042 controller ports */
#define PS2_DATA    0x60
#define PS2_STATUS  0x64
#define PS2_COMMAND 0x64

/* Cursor sprite dimensions */
#define CURSOR_W 12
#define CURSOR_H 19

/* Background save buffer */
static uint32_t cursor_bg[CURSOR_W * CURSOR_H];
static int32_t  cursor_bg_x = -1, cursor_bg_y = -1;
static int      cursor_bg_valid = 0;
static int      cursor_visible = 0;

/* Mouse state */
static mouse_state_t mouse_state;

/* PS/2 packet assembly */
static uint8_t mouse_cycle = 0;
static uint8_t mouse_packet[3];

/*
 * Arrow cursor bitmap (12x19).
 * 0 = transparent, 1 = white fill, 2 = black outline
 */
static const uint8_t cursor_bitmap[CURSOR_H][CURSOR_W] = {
    {2,0,0,0,0,0,0,0,0,0,0,0},
    {2,2,0,0,0,0,0,0,0,0,0,0},
    {2,1,2,0,0,0,0,0,0,0,0,0},
    {2,1,1,2,0,0,0,0,0,0,0,0},
    {2,1,1,1,2,0,0,0,0,0,0,0},
    {2,1,1,1,1,2,0,0,0,0,0,0},
    {2,1,1,1,1,1,2,0,0,0,0,0},
    {2,1,1,1,1,1,1,2,0,0,0,0},
    {2,1,1,1,1,1,1,1,2,0,0,0},
    {2,1,1,1,1,1,1,1,1,2,0,0},
    {2,1,1,1,1,1,1,1,1,1,2,0},
    {2,1,1,1,1,1,1,2,2,2,2,2},
    {2,1,1,1,2,1,1,2,0,0,0,0},
    {2,1,1,2,0,2,1,1,2,0,0,0},
    {2,1,2,0,0,2,1,1,2,0,0,0},
    {2,2,0,0,0,0,2,1,1,2,0,0},
    {2,0,0,0,0,0,2,1,1,2,0,0},
    {0,0,0,0,0,0,0,2,1,2,0,0},
    {0,0,0,0,0,0,0,2,2,0,0,0},
};

/* ------------------------------------------------------------------ */
/*  PS/2 controller helpers                                           */
/* ------------------------------------------------------------------ */

static void ps2_wait_write(void) {
    int timeout = 100000;
    while ((inb(PS2_STATUS) & 0x02) && --timeout > 0)
        ;
}

static void ps2_wait_read(void) {
    int timeout = 100000;
    while (!(inb(PS2_STATUS) & 0x01) && --timeout > 0)
        ;
}

static void ps2_send_mouse(uint8_t byte) {
    ps2_wait_write();
    outb(PS2_COMMAND, 0xD4);   /* next byte goes to auxiliary device */
    ps2_wait_write();
    outb(PS2_DATA, byte);
}

static uint8_t ps2_read_data(void) {
    ps2_wait_read();
    return inb(PS2_DATA);
}

/* ------------------------------------------------------------------ */
/*  Cursor save / restore / draw                                      */
/* ------------------------------------------------------------------ */

static void cursor_save_bg(int32_t x, int32_t y) {
    if (!fb_info.available) return;
    uint32_t bpp = fb_info.bpp / 8;

    for (int row = 0; row < CURSOR_H; row++) {
        int32_t sy = y + row;
        for (int col = 0; col < CURSOR_W; col++) {
            int32_t sx = x + col;
            if (sx < 0 || sx >= (int32_t)fb_info.width ||
                sy < 0 || sy >= (int32_t)fb_info.height) {
                cursor_bg[row * CURSOR_W + col] = 0;
                continue;
            }
            volatile uint8_t *p = (volatile uint8_t *)
                (fb_info.virt_addr + sy * fb_info.pitch + sx * bpp);
            cursor_bg[row * CURSOR_W + col] = *(volatile uint32_t *)p;
        }
    }
    cursor_bg_x = x;
    cursor_bg_y = y;
    cursor_bg_valid = 1;
}

static void cursor_restore_bg(void) {
    if (!cursor_bg_valid || !fb_info.available) return;
    uint32_t bpp = fb_info.bpp / 8;

    for (int row = 0; row < CURSOR_H; row++) {
        int32_t sy = cursor_bg_y + row;
        if (sy < 0 || sy >= (int32_t)fb_info.height) continue;
        for (int col = 0; col < CURSOR_W; col++) {
            int32_t sx = cursor_bg_x + col;
            if (sx < 0 || sx >= (int32_t)fb_info.width) continue;
            if (cursor_bitmap[row][col] == 0) continue;  /* was transparent */
            volatile uint8_t *p = (volatile uint8_t *)
                (fb_info.virt_addr + sy * fb_info.pitch + sx * bpp);
            *(volatile uint32_t *)p = cursor_bg[row * CURSOR_W + col];
        }
    }
    cursor_bg_valid = 0;
}

static void cursor_draw(int32_t x, int32_t y) {
    if (!fb_info.available) return;
    uint32_t white = fb_pack_color(255, 255, 255);
    uint32_t black = fb_pack_color(0, 0, 0);
    uint32_t bpp = fb_info.bpp / 8;

    for (int row = 0; row < CURSOR_H; row++) {
        int32_t sy = y + row;
        if (sy < 0 || sy >= (int32_t)fb_info.height) continue;
        for (int col = 0; col < CURSOR_W; col++) {
            int32_t sx = x + col;
            if (sx < 0 || sx >= (int32_t)fb_info.width) continue;
            uint8_t val = cursor_bitmap[row][col];
            if (val == 0) continue;  /* transparent */
            uint32_t color = (val == 1) ? white : black;
            volatile uint8_t *p = (volatile uint8_t *)
                (fb_info.virt_addr + sy * fb_info.pitch + sx * bpp);
            *(volatile uint32_t *)p = color;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Public cursor API                                                  */
/* ------------------------------------------------------------------ */

void mouse_update_cursor(void) {
    if (!cursor_visible) return;
    cursor_restore_bg();
    cursor_save_bg(mouse_state.x, mouse_state.y);
    cursor_draw(mouse_state.x, mouse_state.y);
}

void mouse_show_cursor(void) {
    if (!fb_info.available) return;
    cursor_visible = 1;
    mouse_update_cursor();
}

void mouse_hide_cursor(void) {
    if (!fb_info.available) return;
    cursor_restore_bg();
    cursor_visible = 0;
}

mouse_state_t mouse_get_state(void) {
    return mouse_state;
}

/* ------------------------------------------------------------------ */
/*  IRQ12 handler                                                      */
/* ------------------------------------------------------------------ */

static void mouse_irq(trapframe *r) {
    (void)r;

    uint8_t status = inb(PS2_STATUS);
    /* Bit 5: auxiliary output buffer full (mouse data) */
    if (!(status & 0x20)) return;

    uint8_t data = inb(PS2_DATA);

    switch (mouse_cycle) {
    case 0:
        /* Byte 0 must have bit 3 set (PS/2 always-1 bit) */
        if (!(data & 0x08)) return;  /* out of sync — discard */
        mouse_packet[0] = data;
        mouse_cycle = 1;
        break;
    case 1:
        mouse_packet[1] = data;
        mouse_cycle = 2;
        break;
    case 2: {
        mouse_packet[2] = data;
        mouse_cycle = 0;

        uint8_t flags = mouse_packet[0];
        int32_t dx = (int32_t)mouse_packet[1];
        int32_t dy = (int32_t)mouse_packet[2];

        /* Sign extend */
        if (flags & 0x10) dx |= (int32_t)0xFFFFFF00;
        if (flags & 0x20) dy |= (int32_t)0xFFFFFF00;

        /* Discard overflow */
        if (flags & 0x40) dx = 0;
        if (flags & 0x80) dy = 0;

        /* PS/2 Y is inverted (positive = up), flip for screen coords */
        dy = -dy;

        uint8_t old_buttons = mouse_state.buttons;
        mouse_state.x += dx;
        mouse_state.y += dy;

        /* Clamp to screen */
        if (mouse_state.x < 0) mouse_state.x = 0;
        if (mouse_state.y < 0) mouse_state.y = 0;
        if (fb_info.available) {
            if (mouse_state.x >= (int32_t)fb_info.width)
                mouse_state.x = (int32_t)fb_info.width - 1;
            if (mouse_state.y >= (int32_t)fb_info.height)
                mouse_state.y = (int32_t)fb_info.height - 1;
        }

        mouse_state.buttons = flags & 0x07;

        /* Update cursor and push move event */
        if (dx != 0 || dy != 0) {
            mouse_update_cursor();
            event_push_mouse_move(mouse_state.x, mouse_state.y, dx, dy);
        }

        /* Check for button changes */
        uint8_t changed = old_buttons ^ mouse_state.buttons;
        for (int btn = 0; btn < 3; btn++) {
            if (changed & (1 << btn)) {
                uint8_t pressed = (mouse_state.buttons >> btn) & 1;
                event_push_mouse_button(mouse_state.x, mouse_state.y,
                                        (uint8_t)(1 << btn), pressed);
            }
        }
        break;
    }
    }
}

/* ------------------------------------------------------------------ */
/*  Initialization                                                     */
/* ------------------------------------------------------------------ */

void mouse_init(void) {
    if (!fb_info.available) return;

    /* Start cursor at screen center */
    mouse_state.x = (int32_t)(fb_info.width / 2);
    mouse_state.y = (int32_t)(fb_info.height / 2);
    mouse_state.buttons = 0;
    mouse_cycle = 0;

    /* 1. Enable auxiliary device (mouse) */
    ps2_wait_write();
    outb(PS2_COMMAND, 0xA8);

    /* 2. Read controller configuration byte */
    ps2_wait_write();
    outb(PS2_COMMAND, 0x20);
    uint8_t config = ps2_read_data();

    /* 3. Enable IRQ12, ensure mouse clock enabled */
    config |= 0x02;    /* bit 1: enable IRQ12 */
    config &= ~0x20;   /* bit 5: clear = mouse clock enabled */

    /* 4. Write updated configuration back */
    ps2_wait_write();
    outb(PS2_COMMAND, 0x60);
    ps2_wait_write();
    outb(PS2_DATA, config);

    /* 5. Reset mouse */
    ps2_send_mouse(0xFF);
    ps2_read_data();   /* ACK (0xFA) */
    ps2_read_data();   /* self-test pass (0xAA) */
    ps2_read_data();   /* mouse ID (0x00) */

    /* 6. Set defaults */
    ps2_send_mouse(0xF6);
    ps2_read_data();   /* ACK */

    /* 7. Enable data reporting (streaming mode) */
    ps2_send_mouse(0xF4);
    ps2_read_data();   /* ACK */

    /* 8. Register IRQ12 handler and unmask */
    irq_install_handler(12, mouse_irq);
    pic_clear_mask(12);

    /* IRQ12 is on the slave PIC — unmask IRQ2 (cascade) so slave
       interrupts can reach the CPU. */
    pic_clear_mask(2);
}
