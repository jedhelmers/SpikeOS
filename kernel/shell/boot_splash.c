#include <kernel/boot_splash.h>
#include <kernel/tty.h>
#include <kernel/keyboard.h>
#include <kernel/key_event.h>
#include <kernel/hal.h>
#include <kernel/framebuffer.h>
#include <kernel/fb_console.h>
#include <stdint.h>
#include <stddef.h>

/* VGA color constants (matching vga.h enum) */
#define COL_BLACK       0
#define COL_GREEN       2
#define COL_CYAN        3
#define COL_DARK_GREY   8
#define COL_LIGHT_GREEN 10

/* CP437 box drawing / block characters */
#define BOX_TL    '\xC9'  /* ╔ */
#define BOX_TR    '\xBB'  /* ╗ */
#define BOX_BL    '\xC8'  /* ╚ */
#define BOX_BR    '\xBC'  /* ╝ */
#define BOX_H     '\xCD'  /* ═ */
#define BOX_V     '\xBA'  /* ║ */
#define LINE_H    '\xC4'  /* ─ */
#define BLOCK     '\xDB'  /* █ */
#define SHADE_LT  '\xB0'  /* ░ */

#define SCREEN_W  80
#define SCREEN_H  25

static inline uint8_t mkcolor(uint8_t fg, uint8_t bg) {
    return fg | (bg << 4);
}

static void splash_delay(uint32_t n) {
    for (volatile uint32_t i = 0; i < n; i++);
}

#define DELAY_SHORT   2000000
#define DELAY_MED     5000000
#define DELAY_LONG    9000000
#define DELAY_TINY     500000

/* Framebuffer centering offsets (character grid units) */
static uint32_t fb_off_x;
static uint32_t fb_off_y;
static int use_fb_splash;

/* Core rendering primitive — dispatches to FB or VGA */
static void splash_put(char ch, uint8_t vga_color, size_t x, size_t y) {
    if (use_fb_splash) {
        uint8_t fg_idx = vga_color & 0x0F;
        uint8_t bg_idx = (vga_color >> 4) & 0x0F;
        fb_render_char((uint32_t)x + fb_off_x, (uint32_t)y + fb_off_y,
                       (uint8_t)ch, fb_vga_color(fg_idx), fb_vga_color(bg_idx));
    } else {
        terminal_putentryat(ch, vga_color, x, y);
    }
}

/* Write a string at (x, y) with given color — no cursor movement */
static void splash_puts(size_t x, size_t y, uint8_t color, const char *s) {
    while (*s) {
        splash_put(*s, color, x, y);
        s++;
        x++;
    }
}

/* Fill a horizontal run with a character */
static void splash_fill(size_t x, size_t y, size_t len, char ch, uint8_t color) {
    for (size_t i = 0; i < len; i++)
        splash_put(ch, color, x + i, y);
}

/* Clear entire screen to black */
static void splash_clear(void) {
    if (use_fb_splash) {
        fb_clear(0);
    } else {
        uint8_t black = mkcolor(COL_BLACK, COL_BLACK);
        for (size_t y = 0; y < SCREEN_H; y++)
            for (size_t x = 0; x < SCREEN_W; x++)
                terminal_putentryat(' ', black, x, y);
    }
}

/* Draw double-line border */
static void draw_border(uint8_t color) {
    /* Corners */
    splash_put(BOX_TL, color, 0, 0);
    splash_put(BOX_TR, color, SCREEN_W - 1, 0);
    splash_put(BOX_BL, color, 0, SCREEN_H - 1);
    splash_put(BOX_BR, color, SCREEN_W - 1, SCREEN_H - 1);

    /* Horizontal edges */
    for (size_t x = 1; x < SCREEN_W - 1; x++) {
        splash_put(BOX_H, color, x, 0);
        splash_put(BOX_H, color, x, SCREEN_H - 1);
    }

    /* Vertical edges */
    for (size_t y = 1; y < SCREEN_H - 1; y++) {
        splash_put(BOX_V, color, 0, y);
        splash_put(BOX_V, color, SCREEN_W - 1, y);
    }
}

/*
 * "SPIKE OS" logo — 5 rows x 42 columns
 * '#' = full block character, ' ' = empty
 */
static const char *logo_rows[5] = {
    "#####  ####  ###  #  #  ####   ####  #####",
    "#      #  #   #   # #   #      #  #  #    ",
    "#####  ####   #   ##    ###    #  #  #####",
    "    #  #      #   # #   #      #  #      #",
    "#####  #     ###  #  #  ####   ####  #####",
};
#define LOGO_W    42
#define LOGO_H    5
#define LOGO_X    ((SCREEN_W - LOGO_W) / 2)
#define LOGO_Y    4

static void draw_logo(uint8_t color) {
    for (int row = 0; row < LOGO_H; row++) {
        const char *line = logo_rows[row];
        for (int col = 0; col < LOGO_W; col++) {
            if (line[col] == '#')
                splash_put(BLOCK, color, LOGO_X + col, LOGO_Y + row);
        }
        splash_delay(DELAY_LONG);
    }
}

/* Draw progress bar: [████░░░░░░] at given row */
#define BAR_X       6
#define BAR_W       50
#define BAR_ROW     19

static void draw_progress_frame(uint8_t color) {
    splash_put('[', color, BAR_X - 1, BAR_ROW);
    splash_put(']', color, BAR_X + BAR_W, BAR_ROW);
    /* Fill empty */
    splash_fill(BAR_X, BAR_ROW, BAR_W, SHADE_LT, mkcolor(COL_DARK_GREY, COL_BLACK));
}

static void fill_progress(int target_pct, uint8_t color) {
    int target_chars = (target_pct * BAR_W) / 100;
    /* Count currently filled chars */
    static int current_chars = 0;

    for (int i = current_chars; i < target_chars; i++) {
        splash_put(BLOCK, color, BAR_X + i, BAR_ROW);
        splash_delay(DELAY_TINY);
    }
    current_chars = target_chars;

    /* Update percentage text */
    char pct_buf[8];
    int p = target_pct;
    int pos = 0;
    if (p >= 100) { pct_buf[pos++] = '1'; pct_buf[pos++] = '0'; pct_buf[pos++] = '0'; }
    else if (p >= 10) { pct_buf[pos++] = '0' + (p / 10); pct_buf[pos++] = '0' + (p % 10); }
    else { pct_buf[pos++] = ' '; pct_buf[pos++] = '0' + p; }
    pct_buf[pos++] = '%';
    pct_buf[pos] = '\0';
    splash_puts(BAR_X + BAR_W + 2, BAR_ROW, mkcolor(COL_GREEN, COL_BLACK), pct_buf);
}

/* Stage check messages */
static const char *stage_msgs[] = {
    "Memory check",
    "I/O subsystem",
    "Filesystem",
    "Kernel services",
};
#define STAGE_COUNT 4
#define STAGE_X     5
#define STAGE_Y     14
#define DOT_END     60
#define OK_X        62

static void draw_stage(int idx, uint8_t text_color, uint8_t ok_color, uint8_t dot_color) {
    size_t y = STAGE_Y + idx;
    const char *msg = stage_msgs[idx];

    /* Draw "> " prefix */
    splash_puts(STAGE_X, y, text_color, "> ");

    /* Draw message text */
    size_t x = STAGE_X + 2;
    while (*msg) {
        splash_put(*msg, text_color, x, y);
        msg++;
        x++;
    }

    /* Animate dots */
    for (size_t dx = x; dx <= DOT_END; dx++) {
        splash_put('.', dot_color, dx, y);
        splash_delay(DELAY_TINY / 4);
    }

    splash_delay(DELAY_SHORT);

    /* Draw [ OK ] */
    splash_puts(OK_X, y, ok_color, "[  OK  ]");
}

void boot_splash(void) {
    /* Detect framebuffer and compute centering offsets */
    if (fb_info.available) {
        use_fb_splash = 1;
        fb_off_x = (fb_info.width / 8 - SCREEN_W) / 2;
        fb_off_y = (fb_info.height / 16 - SCREEN_H) / 2;
    } else {
        use_fb_splash = 0;
    }

    uint8_t border_color = mkcolor(COL_CYAN,        COL_BLACK);
    uint8_t logo_color   = mkcolor(COL_LIGHT_GREEN, COL_BLACK);
    uint8_t ver_color    = mkcolor(COL_DARK_GREY,   COL_BLACK);
    uint8_t sep_color    = mkcolor(COL_DARK_GREY,   COL_BLACK);
    uint8_t text_color   = mkcolor(COL_GREEN,       COL_BLACK);
    uint8_t ok_color     = mkcolor(COL_LIGHT_GREEN, COL_BLACK);
    uint8_t dot_color    = mkcolor(COL_DARK_GREY,   COL_BLACK);
    uint8_t bar_color    = mkcolor(COL_LIGHT_GREEN, COL_BLACK);
    uint8_t ready_color  = mkcolor(COL_LIGHT_GREEN, COL_BLACK);

    /* 1. Clear and draw border */
    splash_clear();
    draw_border(border_color);
    splash_delay(DELAY_LONG);

    /* 2. Draw logo row by row */
    draw_logo(logo_color);
    splash_delay(DELAY_MED);

    /* 3. Version text (centered) */
    {
        const char *ver = "System Version 1.0    (c) 2026";
        int ver_len = 0;
        const char *p = ver;
        while (*p++) ver_len++;
        int vx = (SCREEN_W - ver_len) / 2;
        splash_puts(vx, 10, ver_color, ver);
    }
    splash_delay(DELAY_LONG);

    /* 4. Separator line */
    splash_fill(5, 12, 70, LINE_H, sep_color);
    splash_delay(DELAY_LONG);

    /* 5. Progress bar frame */
    draw_progress_frame(mkcolor(COL_GREEN, COL_BLACK));

    /* 6. System check stages with progress */
    for (int i = 0; i < STAGE_COUNT; i++) {
        draw_stage(i, text_color, ok_color, dot_color);
        fill_progress((i + 1) * 25, bar_color);
        splash_delay(DELAY_LONG);
    }

    splash_delay(DELAY_MED);

    /* 7. SYSTEM READY message */
    {
        const char *ready = "*** SYSTEM READY ***";
        int rlen = 0;
        const char *p = ready;
        while (*p++) rlen++;
        int rx = (SCREEN_W - rlen) / 2;
        splash_puts(rx, 21, ready_color, ready);
    }

    splash_delay(DELAY_LONG);

    /* 8. "Press any key to continue..." — wait for keyboard input */
    {
        const char *prompt = "Press any key to continue...";
        int plen = 0;
        const char *p = prompt;
        while (*p++) plen++;
        int px = (SCREEN_W - plen) / 2;
        splash_puts(px, 23, mkcolor(COL_DARK_GREY, COL_BLACK), prompt);

        /* Enable interrupts so the keyboard IRQ can fire */
        hal_irq_enable();

        /* Wait for any key press */
        for (;;) {
            key_event_t ev = keyboard_get_event();
            if (ev.type != KEY_NONE) break;
            hal_halt();
        }

        /* Leave interrupts enabled — shell_readline needs them for
           keyboard IRQ to fire and wake HLT. */
    }
}
