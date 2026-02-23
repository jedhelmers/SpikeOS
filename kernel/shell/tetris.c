#include <kernel/tetris.h>
#include <kernel/vga13.h>
#include <kernel/keyboard.h>
#include <kernel/timer.h>
#include <stdint.h>
#include <string.h>

/* =========================================================================
 * Layout constants
 * ========================================================================= */

#define BOARD_COLS   10
#define BOARD_ROWS   20
#define CELL          8   /* pixels per cell */

/* Board top-left corner on the 320x200 screen */
#define BX  110
#define BY   20

/* Info panel origin */
#define IX  210
#define IY   20

/* =========================================================================
 * Palette  (DAC values: R/G/B each 0-63)
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

static void setup_palette(void) {
    for (int i = 0; i < 11; i++)
        vga13_set_palette((uint8_t)i, PAL[i][0], PAL[i][1], PAL[i][2]);
}

/* =========================================================================
 * 5x7 pixel font for digits 0-9  (each row = 5 bits, MSB left)
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

/* Draw digit d at pixel position (px, py) in the given colour */
static void draw_digit(int px, int py, int d, uint8_t col) {
    if (d < 0 || d > 9) return;
    const uint8_t *glyph = FONT5X7[d];
    for (int row = 0; row < 7; row++) {
        for (int bit = 4; bit >= 0; bit--) {
            uint8_t c = (glyph[row] >> bit) & 1 ? col : COL_BG;
            vga13_putpixel(px + (4 - bit), py + row, c);
        }
    }
}

/* Draw a decimal number (up to 7 digits) at (px, py) */
static void draw_number(int px, int py, uint32_t n, uint8_t col) {
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
        draw_digit(px + i * 7, py, (int)(uint8_t)buf[i], col);
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
    vga13_fill_rect(BX + col * CELL, BY + row * CELL, CELL - 1, CELL - 1, color);
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
    vga13_fill_rect(BX - 2, BY, 2, BOARD_ROWS * CELL, COL_BORDER);
    /* right wall */
    vga13_fill_rect(BX + BOARD_COLS * CELL, BY, 2, BOARD_ROWS * CELL, COL_BORDER);
    /* bottom */
    vga13_fill_rect(BX - 2, BY + BOARD_ROWS * CELL, BOARD_COLS * CELL + 4, 2, COL_BORDER);
}

/* Paint the board area with the grid colour once.
   draw_cell() fills CELL-1 × CELL-1 leaving the 1 px borders in COL_GRID. */
static void draw_board_bg(void) {
    vga13_fill_rect(BX, BY, BOARD_COLS * CELL, BOARD_ROWS * CELL, COL_GRID);
}

/* Draw the info panel labels + values */
static void draw_info(const tetris_t *g) {
    /* SCORE label */
    draw_digit(IX,      IY,      5, COL_WHITE);
    draw_digit(IX + 7,  IY,      2, COL_WHITE);
    draw_digit(IX + 14, IY,      0, COL_WHITE);
    draw_digit(IX + 21, IY,      8, COL_WHITE);
    draw_digit(IX + 28, IY,      3, COL_WHITE);
    /* score value */
    draw_number(IX, IY + 10, g->score, COL_CYAN);

    /* LEVEL label */
    draw_digit(IX,      IY + 30, 7, COL_WHITE);
    draw_digit(IX + 7,  IY + 30, 3, COL_WHITE);
    draw_digit(IX + 14, IY + 30, 5, COL_WHITE);
    draw_digit(IX + 21, IY + 30, 3, COL_WHITE);
    draw_digit(IX + 28, IY + 30, 7, COL_WHITE);
    /* level value */
    draw_number(IX, IY + 40, (uint32_t)g->level, COL_GREEN);

    /* LINES label */
    draw_digit(IX,      IY + 60, 7, COL_WHITE);
    draw_digit(IX + 7,  IY + 60, 1, COL_WHITE);
    draw_digit(IX + 14, IY + 60, 1, COL_WHITE);
    draw_digit(IX + 21, IY + 60, 2, COL_WHITE);
    draw_digit(IX + 28, IY + 60, 5, COL_WHITE);
    /* lines value */
    draw_number(IX, IY + 70, (uint32_t)g->lines, COL_YELLOW);

    /* NEXT label */
    draw_digit(IX,      IY + 100, 5, COL_WHITE);
    draw_digit(IX + 7,  IY + 100, 3, COL_WHITE);
    draw_digit(IX + 14, IY + 100, 1, COL_WHITE);
    draw_digit(IX + 21, IY + 100, 4, COL_WHITE);

    /* next piece preview (4x4 box) */
    vga13_fill_rect(IX, IY + 110, 4 * CELL, 4 * CELL, COL_BG);
    uint8_t nc = piece_color(g->next);
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            if (piece_cell(g->next, 0, r, c))
                vga13_fill_rect(IX + c * CELL, IY + 110 + r * CELL,
                                CELL - 1, CELL - 1, nc);
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
        /* erase old position from board render, update coords */
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
    draw_digit(BX + 10, BY + 70,  6, COL_RED);
    draw_digit(BX + 20, BY + 70,  0, COL_RED);
    draw_digit(BX + 30, BY + 70,  7, COL_RED);
    draw_digit(BX + 40, BY + 70,  3, COL_RED);

    draw_digit(BX + 10, BY + 80,  0, COL_WHITE);
    draw_digit(BX + 20, BY + 80,  5, COL_WHITE);
    draw_digit(BX + 30, BY + 80,  3, COL_WHITE);
    draw_digit(BX + 40, BY + 80,  8, COL_WHITE);
}

/* =========================================================================
 * Main entry point
 * ========================================================================= */

void tetris_run(void) {
    vga13_enter();
    setup_palette();
    vga13_clear(COL_BG);
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

    vga13_exit();
}
