#include <kernel/tetris.h>
#include <kernel/framebuffer.h>
#include <kernel/window.h>
#include <kernel/fb_console.h>
#include <kernel/keyboard.h>
#include <kernel/timer.h>
#include <stdint.h>
#include <string.h>

/* =========================================================================
 * Layout constants
 * ========================================================================= */

#define BOARD_COLS   10
#define BOARD_ROWS   20
#define CELL         16   /* pixels per cell (2x original for framebuffer) */

/* Board top-left corner relative to window content area */
#define BX    0
#define BY    8

/* Info panel origin relative to window content area */
#define IX  170
#define IY    8

/* Window outer dimensions — chosen so content area works out exactly:
   content_w = (322-2) = 320, snapped to 8 → 320
   content_h = (358-22) = 336, snapped to 16 → 336 */
#define TETRIS_WIN_W  322
#define TETRIS_WIN_H  358

/* =========================================================================
 * Palette  (DAC values: R/G/B each 0-63, scaled to 0-255 for framebuffer)
 * ========================================================================= */

#define COL_BG      0
#define COL_CYAN    1   /* I */
#define COL_YELLOW  2   /* O */
#define COL_MAGENTA 3   /* T */
#define COL_GREEN   4   /* S */
#define COL_RED     5   /* Z */
#define COL_BLUE    6   /* J */
#define COL_ORANGE  7   /* L */
#define COL_BORDER  8
#define COL_WHITE   9
#define COL_GRID   10   /* barely-visible grid lines */

static const uint8_t PAL[11][3] = {
    {  0,  0,  0 }, /* 0  black       */
    {  0, 42, 42 }, /* 1  cyan        */
    { 42, 42,  0 }, /* 2  yellow      */
    { 42,  0, 42 }, /* 3  magenta     */
    {  0, 42,  0 }, /* 4  green       */
    { 42,  0,  0 }, /* 5  red         */
    {  0,  0, 42 }, /* 6  blue        */
    { 42, 21,  0 }, /* 7  orange      */
    { 20, 20, 20 }, /* 8  dark grey   */
    { 63, 63, 63 }, /* 9  white       */
    {  7,  7,  7 }, /* 10 grid (dim)  */
};

/* Pre-packed 32-bit framebuffer colors (filled at game start) */
static uint32_t colors[11];

static window_t *tetris_win = NULL;

static void setup_colors(void) {
    for (int i = 0; i < 11; i++)
        colors[i] = fb_pack_color(PAL[i][0] * 4, PAL[i][1] * 4, PAL[i][2] * 4);
}

/* Helper: absolute screen X/Y from content-relative offset */
static inline uint32_t cx(int x) { return tetris_win->content_x + (uint32_t)x; }
static inline uint32_t cy(int y) { return tetris_win->content_y + (uint32_t)y; }

/* =========================================================================
 * 5x7 pixel font for digits 0-9  (each row = 5 bits, MSB left)
 * Scaled 2× for readability on the framebuffer.
 * ========================================================================= */

static const uint8_t FONT5X7[10][7] = {
    { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E }, /* 0 */
    { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E }, /* 1 */
    { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F }, /* 2 */
    { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E }, /* 3 */
    { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 }, /* 4 */
    { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E }, /* 5 */
    { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E }, /* 6 */
    { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 }, /* 7 */
    { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E }, /* 8 */
    { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C }, /* 9 */
};

/* Draw digit d at pixel position (px, py) relative to content area, scaled 2× */
static void draw_digit(int px, int py, int d, uint32_t col) {
    if (d < 0 || d > 9) return;
    const uint8_t *glyph = FONT5X7[d];
    for (int row = 0; row < 7; row++) {
        for (int bit = 4; bit >= 0; bit--) {
            uint32_t c = (glyph[row] >> bit) & 1 ? col : colors[COL_BG];
            fb_fill_rect(cx(px + (4 - bit) * 2), cy(py + row * 2), 2, 2, c);
        }
    }
}

/* Draw a decimal number (up to 7 digits) at (px, py) relative to content */
static void draw_number(int px, int py, uint32_t n, uint32_t col) {
    char buf[8];
    int len = 0;
    if (n == 0) { buf[len++] = 0; }
    else {
        uint32_t tmp = n;
        while (tmp > 0 && len < 7) { buf[len++] = (char)(tmp % 10); tmp /= 10; }
        /* reverse */
        for (int i = 0, j = len - 1; i < j; i++, j--) {
            char t = buf[i]; buf[i] = buf[j]; buf[j] = t;
        }
    }
    for (int i = 0; i < len; i++)
        draw_digit(px + i * 12, py, (int)(uint8_t)buf[i], col);
}

/* =========================================================================
 * Piece definitions
 * bit 15 = row0/col0 of the 4x4 bounding box, bit 0 = row3/col3
 * ========================================================================= */

static const uint16_t SHAPES[7][4] = {
    { 0x0F00, 0x2222, 0x00F0, 0x4444 }, /* I – cyan    */
    { 0x6600, 0x6600, 0x6600, 0x6600 }, /* O – yellow  */
    { 0x0E40, 0x4C40, 0x4E00, 0x4640 }, /* T – magenta */
    { 0x06C0, 0x8C40, 0x06C0, 0x8C40 }, /* S – green   */
    { 0x0C60, 0x4C80, 0x0C60, 0x4C80 }, /* Z – red     */
    { 0x44C0, 0x8E00, 0xC880, 0x0E20 }, /* J – blue    */
    { 0x4460, 0x0E80, 0x6440, 0x02E0 }, /* L – orange  */
};

/* Piece colour = piece index + 1 (maps to palette entries 1-7) */
static inline uint8_t piece_color(int p) { return (uint8_t)(p + 1); }

/* Return 1 if cell (r,c) of piece p at rotation rot is filled */
static int piece_cell(int p, int rot, int r, int c) {
    return (SHAPES[p][rot] >> (15 - r * 4 - c)) & 1;
}

/* =========================================================================
 * Game state
 * ========================================================================= */

typedef struct {
    uint8_t board[BOARD_ROWS][BOARD_COLS]; /* 0=empty, 1-7=colour */
    int     px, py;  /* current piece top-left in board coords     */
    int     piece;   /* current piece index 0-6                    */
    int     rot;     /* current rotation 0-3                       */
    int     next;    /* next piece index                           */
    uint32_t score;
    int     level;
    int     lines;
    int     alive;
} tetris_t;

/* =========================================================================
 * Simple LCG for pseudo-random piece selection
 * ========================================================================= */

static uint32_t rng_state = 12345;

static uint32_t rng_next(void) {
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state;
}

static int rand_piece(void) {
    return (int)(rng_next() % 7);
}

/* =========================================================================
 * Collision detection
 * ========================================================================= */

static int fits(const tetris_t *g, int px, int py, int rot) {
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            if (!piece_cell(g->piece, rot, r, c)) continue;
            int bx = px + c;
            int by = py + r;
            if (bx < 0 || bx >= BOARD_COLS || by >= BOARD_ROWS) return 0;
            if (by >= 0 && g->board[by][bx]) return 0;
        }
    }
    return 1;
}

/* =========================================================================
 * Drawing helpers
 * ========================================================================= */

static void draw_cell(int col, int row, uint8_t color) {
    fb_fill_rect(cx(BX + col * CELL), cy(BY + row * CELL),
                 CELL - 1, CELL - 1, colors[color]);
}

static void draw_board(const tetris_t *g) {
    for (int r = 0; r < BOARD_ROWS; r++)
        for (int c = 0; c < BOARD_COLS; c++)
            draw_cell(c, r, g->board[r][c]);
}

static void draw_piece(const tetris_t *g, uint8_t col_override, int use_override) {
    uint8_t col = use_override ? col_override : piece_color(g->piece);
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            if (piece_cell(g->piece, g->rot, r, c)) {
                int br = g->py + r;
                int bc = g->px + c;
                if (br >= 0 && br < BOARD_ROWS && bc >= 0 && bc < BOARD_COLS)
                    draw_cell(bc, br, col);
            }
}

static void draw_border(void) {
    /* left wall */
    fb_fill_rect(cx(BX - 2), cy(BY), 2, BOARD_ROWS * CELL, colors[COL_BORDER]);
    /* right wall */
    fb_fill_rect(cx(BX + BOARD_COLS * CELL), cy(BY), 2, BOARD_ROWS * CELL, colors[COL_BORDER]);
    /* bottom */
    fb_fill_rect(cx(BX - 2), cy(BY + BOARD_ROWS * CELL),
                 BOARD_COLS * CELL + 4, 2, colors[COL_BORDER]);
}

/* Paint the board area with the grid colour once.
   draw_cell() fills CELL-1 × CELL-1 leaving the 1 px borders in COL_GRID. */
static void draw_board_bg(void) {
    fb_fill_rect(cx(BX), cy(BY),
                 BOARD_COLS * CELL, BOARD_ROWS * CELL, colors[COL_GRID]);
}

/* Draw the info panel labels + values */
static void draw_info(const tetris_t *g) {
    /* SCORE label (digits used as character codes: S=5, C=2, O=0, R=8, E=3) */
    draw_digit(IX,      IY,      5, colors[COL_WHITE]);
    draw_digit(IX + 12, IY,      2, colors[COL_WHITE]);
    draw_digit(IX + 24, IY,      0, colors[COL_WHITE]);
    draw_digit(IX + 36, IY,      8, colors[COL_WHITE]);
    draw_digit(IX + 48, IY,      3, colors[COL_WHITE]);
    /* score value */
    draw_number(IX, IY + 18, g->score, colors[COL_CYAN]);

    /* LEVEL label (L=7, E=3, V=5, E=3, L=7) */
    draw_digit(IX,      IY + 50, 7, colors[COL_WHITE]);
    draw_digit(IX + 12, IY + 50, 3, colors[COL_WHITE]);
    draw_digit(IX + 24, IY + 50, 5, colors[COL_WHITE]);
    draw_digit(IX + 36, IY + 50, 3, colors[COL_WHITE]);
    draw_digit(IX + 48, IY + 50, 7, colors[COL_WHITE]);
    /* level value */
    draw_number(IX, IY + 68, (uint32_t)g->level, colors[COL_GREEN]);

    /* LINES label (L=7, I=1, N=1, E=2, S=5) */
    draw_digit(IX,      IY + 100, 7, colors[COL_WHITE]);
    draw_digit(IX + 12, IY + 100, 1, colors[COL_WHITE]);
    draw_digit(IX + 24, IY + 100, 1, colors[COL_WHITE]);
    draw_digit(IX + 36, IY + 100, 2, colors[COL_WHITE]);
    draw_digit(IX + 48, IY + 100, 5, colors[COL_WHITE]);
    /* lines value */
    draw_number(IX, IY + 118, (uint32_t)g->lines, colors[COL_YELLOW]);

    /* NEXT label (N=5, E=3, X=1, T=4) */
    draw_digit(IX,      IY + 160, 5, colors[COL_WHITE]);
    draw_digit(IX + 12, IY + 160, 3, colors[COL_WHITE]);
    draw_digit(IX + 24, IY + 160, 1, colors[COL_WHITE]);
    draw_digit(IX + 36, IY + 160, 4, colors[COL_WHITE]);

    /* next piece preview (4×4 box) */
    fb_fill_rect(cx(IX), cy(IY + 178), 4 * CELL, 4 * CELL, colors[COL_BG]);
    uint8_t nc = piece_color(g->next);
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            if (piece_cell(g->next, 0, r, c))
                fb_fill_rect(cx(IX + c * CELL), cy(IY + 178 + r * CELL),
                             CELL - 1, CELL - 1, colors[nc]);
}

static void render(const tetris_t *g) {
    draw_board(g);
    draw_piece(g, 0, 0);
    draw_info(g);
}

/* =========================================================================
 * Game logic
 * ========================================================================= */

static void spawn_piece(tetris_t *g) {
    g->piece = g->next;
    g->next  = rand_piece();
    g->rot   = 0;
    g->px    = BOARD_COLS / 2 - 2;
    g->py    = 0;
    if (!fits(g, g->px, g->py, g->rot))
        g->alive = 0; /* game over — spawn blocked */
}

static void tetris_init(tetris_t *g) {
    memset(g->board, 0, sizeof(g->board));
    g->score = 0;
    g->level = 0;
    g->lines = 0;
    g->alive = 1;
    g->next  = rand_piece();
    spawn_piece(g);
}

static int try_move(tetris_t *g, int dx, int dy) {
    if (fits(g, g->px + dx, g->py + dy, g->rot)) {
        g->px += dx;
        g->py += dy;
        return 1;
    }
    return 0;
}

static void try_rotate(tetris_t *g) {
    int nr = (g->rot + 1) % 4;
    /* try natural rotation, then wall-kick left and right */
    if      (fits(g, g->px,     g->py, nr)) g->rot = nr;
    else if (fits(g, g->px - 1, g->py, nr)) { g->px--; g->rot = nr; }
    else if (fits(g, g->px + 1, g->py, nr)) { g->px++; g->rot = nr; }
}

static void lock_piece(tetris_t *g) {
    uint8_t col = piece_color(g->piece);
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            if (piece_cell(g->piece, g->rot, r, c)) {
                int br = g->py + r;
                int bc = g->px + c;
                if (br >= 0 && br < BOARD_ROWS && bc >= 0 && bc < BOARD_COLS)
                    g->board[br][bc] = col;
            }
}

static int clear_lines(tetris_t *g) {
    int cleared = 0;
    for (int r = BOARD_ROWS - 1; r >= 0; ) {
        int full = 1;
        for (int c = 0; c < BOARD_COLS; c++)
            if (!g->board[r][c]) { full = 0; break; }
        if (full) {
            /* shift rows above down */
            for (int row = r; row > 0; row--)
                for (int c = 0; c < BOARD_COLS; c++)
                    g->board[row][c] = g->board[row - 1][c];
            for (int c = 0; c < BOARD_COLS; c++)
                g->board[0][c] = 0;
            cleared++;
            /* don't advance r — check same row again (it shifted) */
        } else {
            r--;
        }
    }
    return cleared;
}

static void hard_drop(tetris_t *g) {
    while (try_move(g, 0, 1))
        ;
}

/* Fall interval in timer ticks (100 Hz).
   Starts at 50 ticks (0.5 s), decreases 4 ticks per level, floor = 4. */
static uint32_t fall_interval(int level) {
    int interval = 50 - level * 4;
    return (uint32_t)(interval < 4 ? 4 : interval);
}

/* =========================================================================
 * Game over screen
 * ========================================================================= */

static void draw_game_over(const tetris_t *g) {
    /* dim the board */
    for (int r = 0; r < BOARD_ROWS; r++)
        for (int c = 0; c < BOARD_COLS; c++)
            if (g->board[r][c])
                draw_cell(c, r, COL_BORDER);

    /* "GAME" in large digits (re-use digit bitmaps as placeholder) */
    draw_digit(BX + 20, BY + 130,  6, colors[COL_RED]);
    draw_digit(BX + 40, BY + 130,  0, colors[COL_RED]);
    draw_digit(BX + 60, BY + 130,  7, colors[COL_RED]);
    draw_digit(BX + 80, BY + 130,  3, colors[COL_RED]);

    draw_digit(BX + 20, BY + 150,  0, colors[COL_WHITE]);
    draw_digit(BX + 40, BY + 150,  5, colors[COL_WHITE]);
    draw_digit(BX + 60, BY + 150,  3, colors[COL_WHITE]);
    draw_digit(BX + 80, BY + 150,  8, colors[COL_WHITE]);
}

/* =========================================================================
 * Main entry point
 * ========================================================================= */

void tetris_run(void) {
    /* Create Tetris window centered on screen */
    int32_t win_x = ((int32_t)fb_info.width  - TETRIS_WIN_W) / 2;
    int32_t win_y = ((int32_t)fb_info.height - TETRIS_WIN_H) / 2 - 40;
    if (win_y < 0) win_y = 0;

    tetris_win = wm_create_window(win_x, win_y, TETRIS_WIN_W, TETRIS_WIN_H, "Tetris");
    if (!tetris_win) return;

    tetris_win->flags &= ~WIN_FLAG_RESIZABLE;  /* fixed-size game window */

    setup_colors();
    wm_draw_chrome(tetris_win);

    /* Clear content area */
    fb_fill_rect(tetris_win->content_x, tetris_win->content_y,
                 tetris_win->content_w, tetris_win->content_h, colors[COL_BG]);

    draw_border();
    draw_board_bg();

    /* Seed RNG with current tick count */
    rng_state = timer_ticks() ^ 0xDEADBEEFu;

    tetris_t g;
    tetris_init(&g);

    /* Initial full render */
    draw_info(&g);
    render(&g);

    uint32_t last_fall = timer_ticks();
    uint32_t last_score = g.score + 1; /* force info redraw on first frame */
    static const uint32_t score_table[5] = { 0, 100, 300, 500, 800 };

    while (g.alive) {
        /* --- Process window manager events (mouse drag, etc.) --- */
        wm_process_events();

        /* --- Input (non-blocking) --- */
        key_event_t k = keyboard_get_event();
        if (k.type == KEY_CHAR) {
            switch (k.ch) {
                case 'a': try_move(&g, -1, 0);  break;
                case 'd': try_move(&g,  1, 0);  break;
                case 'w': try_rotate(&g);        break;
                case 's': try_move(&g,  0, 1);  break;
                case ' ': hard_drop(&g);         break;
                case 'q': g.alive = 0;           break;
            }
        } else if (k.type == KEY_LEFT)   { try_move(&g, -1, 0); }
          else if (k.type == KEY_RIGHT)  { try_move(&g,  1, 0); }
          else if (k.type == KEY_UP)     { try_rotate(&g); }
          else if (k.type == KEY_DOWN)   { try_move(&g,  0, 1); }
          else if (k.type == KEY_CTRL_C) { g.alive = 0; }

        if (!g.alive) break;

        /* --- Gravity --- */
        uint32_t interval = fall_interval(g.level);
        if (timer_ticks() - last_fall >= interval) {
            if (!try_move(&g, 0, 1)) {
                lock_piece(&g);
                int n = clear_lines(&g);
                if (n > 0 && n <= 4) {
                    g.score += score_table[n] * (uint32_t)(g.level + 1);
                    g.lines += n;
                    g.level  = g.lines / 10;
                }
                spawn_piece(&g);
            }
            last_fall = timer_ticks();
        }

        /* --- Render --- */
        render(&g);

        /* Only redraw info panel when values change */
        if (g.score != last_score) {
            draw_info(&g);
            last_score = g.score;
        }

        asm volatile ("hlt");
    }

    if (!g.alive) {
        draw_game_over(&g);
        /* Wait for any key */
        while (keyboard_get_event().type == KEY_NONE)
            asm volatile ("hlt");
    }

    /* Clean up: destroy window, return focus to shell */
    wm_destroy_window(tetris_win);
    tetris_win = NULL;

    window_t *sw = wm_get_shell_window();
    if (sw) {
        wm_focus_window(sw);
        wm_draw_chrome(sw);
        fb_console_repaint();
    }
}
