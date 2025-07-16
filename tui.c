#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "tetris.h"

/* The main loop sleeps for up to one video frame (≈60 fps) when no key is
 * available, then wakes to update AI/physics.
 * 16 ms of idle sleep is negligible compared with typical SSH RTTs. It
 * prevents busy-waiting, so CPU load remains low even on a high-latency link.
 */
#ifndef TUI_POLL_MS
#define TUI_POLL_MS 16
#endif

/* ANSI escape sequences */
#define ESC "\033"
#define CLEAR_SCREEN ESC "[2J" ESC "[1;1H"
#define HIDE_CURSOR ESC "[?25l"
#define SHOW_CURSOR ESC "[?25h"
#define RESET_COLOR ESC "[0m"

/* Background-color helper (0–7 = standard ANSI colors) */
#define bgcolor(c, s) printf("\033[%dm" s, (c) ? ((c) + 40) : 0)
#define COLOR_RESET ESC "[0m"
#define COLOR_BORDER ESC "[1;32m" /* Bright-green border */
#define COLOR_TEXT ESC "[0;37m"   /* White side-bar text */

/* Ghost piece rendering */
#define GHOST_COLOR 9 /* Special sentinel for ghost piece */

/* Falling pieces effect for game over */
#define FALLING_COLS 24  /* More columns for very tight horizontal spacing */
#define FALLING_COLORS 6 /* Colors 2-7 for variety */

/* Layout constraints */
#define MIN_COLS 55
#define MIN_ROWS 21
#define MAX_SHAPES 7 /* I, J, L, O, S, T, Z – exactly seven */

static struct termios orig_termios;
static int ttcols = 80, ttrows = 24; /* Terminal width and height */

/* Off-screen buffers */
static int shadow_board[GRID_HEIGHT][GRID_WIDTH];
static int shadow_preview[4][4];

/* Color for every settled grid cell */
static int color_grid[GRID_HEIGHT][GRID_WIDTH];

/* Double-buffer to draw only what changed */
static int display_buffer[GRID_HEIGHT][GRID_WIDTH];
static bool display_buffer_valid = false;

/* Row-level dirty tracking for optimized rendering */
static bool dirty_row[GRID_HEIGHT];

/* One permanent color for each distinct tetromino shape */
static struct {
    unsigned sig;                 /* Geometry signature (unique per shape) */
    int color;                    /* Value: ANSI background color 2–7 */
} shape_colors[MAX_SHAPES] = {0}; /* One slot per tetromino (7 total) */

static int next_color = 2; /* Round-robin allocator: 2 → 7 → 2 … */

/* Game statistics for sidebar display */
static int current_level = 1;
static int current_points = 0;
static int current_lines_cleared = 0;
static bool current_ai_mode = false;

/* Color preservation for line clearing */
static int preserved_colors[GRID_WIDTH][GRID_HEIGHT];
static int preserved_counts[GRID_WIDTH];

/* Generate unique geometry signature for a shape */
static unsigned shape_signature(const shape_t *s)
{
    if (!s)
        return 0;

    unsigned sig = 0;

    /* Use normalized first rotation for consistent signature */
    for (int i = 0; i < MAX_BLOCK_LEN; i++) {
        int x = s->rot_flat[0][i][0];
        int y = s->rot_flat[0][i][1];

        /* Skip invalid coordinates */
        if (x < 0 || y < 0 || x >= 4 || y >= 4)
            continue;

        sig |= 1u << (y * 4 + x); /* Set bit for each occupied cell */
    }
    return sig;
}

/* Return the persistent color for a shape, assigning one if needed */
int tui_get_shape_color(shape_t *shape)
{
    if (!shape)
        return 2; /* Visible default */

    unsigned sig = shape_signature(shape);

    /* 1. Already mapped? */
    for (int i = 0; i < MAX_SHAPES; i++) {
        if (shape_colors[i].sig == sig && shape_colors[i].sig != 0)
            return shape_colors[i].color;
    }

    /* 2. Allocate the next color in 2–7 (never 8) */
    int assigned_color = next_color;
    next_color = (next_color == 7) ? 2 : next_color + 1;

    /* 3. Store mapping in the first free slot – should always succeed */
    for (int i = 0; i < MAX_SHAPES; i++) {
        if (shape_colors[i].sig == 0) {
            shape_colors[i].sig = sig;
            shape_colors[i].color = assigned_color;
            return assigned_color;
        }
    }

    /* Should never reach here (MAX_SHAPES == 7) */
    return assigned_color;
}

/* Raw-mode helpers */
static void disable_raw_mode(void)
{
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

static void enable_raw_mode(void)
{
    tcgetattr(STDIN_FILENO, &orig_termios);

    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_cflag |= CS8;
    raw.c_oflag &= ~(OPOST);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0; /* 0 = no driver-side delay: snappier keys */

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

/* Terminal-size helper */
static int tty_size(void)
{
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1)
        return -1;

    ttcols = ws.ws_col;
    ttrows = ws.ws_row;

    if (ttcols < MIN_COLS || ttrows < MIN_ROWS) {
        fprintf(stderr, "Terminal too small (min: %dx%d)\n", MIN_COLS,
                MIN_ROWS);
        return -1;
    }

    return 0;
}

/* Convenience helpers */
static void gotoxy(int x, int y)
{
    int xpos = (ttcols - MIN_COLS) / 2;
    int ypos = (ttrows - MIN_ROWS) / 2;

    printf(ESC "[%d;%dH", ypos + y, xpos + x);
}

static void draw_block(int x, int y, int color)
{
    /* Out-of-bounds? Don't draw. */
    if (x < 0 || x >= GRID_WIDTH || y < 1 || y > GRID_HEIGHT)
        return;

    int draw_x = x * 2 + 1;
    int right_border_pos = GRID_WIDTH * 2 + 1;

    if (draw_x >= 1 && draw_x + 1 < right_border_pos) {
        gotoxy(draw_x, y);

        /* Special handling: ghost piece drawn as dim shaded blocks */
        if (color == GHOST_COLOR) {
            printf("\033[2;37m░░" COLOR_RESET); /* Dim white shade */
            return;
        }

        /* Clamp color – any invalid value becomes empty space (color 0) */
        if (color < 0 || color > 7)
            color = 0;

        if (color == 0)
            printf("  "); /* Clear cell */
        else
            bgcolor(color, "  "); /* Two spaces, colored bg */
        printf(COLOR_RESET);
    }
}

/* Test if block can be placed at given offset without collision */
static inline bool block_fits_at_offset(const grid_t *g,
                                        const block_t *b,
                                        int offset_x,
                                        int offset_y)
{
    if (!g || !b || !b->shape)
        return false;

    for (int i = 0; i < MAX_BLOCK_LEN; i++) {
        coord_t cr;
        block_t test_block = *b;
        test_block.offset.x = offset_x;
        test_block.offset.y = offset_y;

        block_get(&test_block, i, &cr);

        /* Skip invalid coordinates */
        if (cr.x < 0 || cr.y < 0)
            continue;

        /* Check bounds */
        if (cr.x >= GRID_WIDTH || cr.y >= GRID_HEIGHT)
            return false;

        /* Check collision with settled blocks */
        if (cr.y >= 0 && g->rows[cr.y][cr.x])
            return false;
    }
    return true;
}

/* Calculate ghost piece position for current falling block */
static int calculate_ghost_y(const grid_t *g, const block_t *falling_block)
{
    if (!g || !falling_block || !falling_block->shape)
        return falling_block->offset.y;

    int ghost_y = falling_block->offset.y;

    /* Drop until collision */
    while (block_fits_at_offset(g, falling_block, falling_block->offset.x,
                                ghost_y - 1))
        ghost_y--;

    return ghost_y;
}

/* Color-grid helpers (settled blocks) */
static void tui_set_block_color(int x, int y, int color)
{
    if (x >= 0 && x < GRID_WIDTH && y >= 0 && y < GRID_HEIGHT) {
        if (color < 2 || color > 7)
            color = 2;
        color_grid[y][x] = color;
    }
}

int tui_get_block_color(int x, int y)
{
    if (x >= 0 && x < GRID_WIDTH && y >= 0 && y < GRID_HEIGHT)
        return color_grid[y][x];
    return 0;
}

/* Draw a colorful Tetris shape at given position for falling pieces effect */
static void draw_falling_shape(shape_t *shape,
                               int base_x,
                               int base_y,
                               int color,
                               int intensity)
{
    if (!shape)
        return;

    /* Apply intensity effects to the color */
    if (intensity == 0) {
        /* Head - bright white override for visibility */
        printf("\033[1;37m");
    } else if (intensity < 2) {
        /* Near head - bright version of the color */
        printf("\033[1;%dm", 30 + color);
    } else if (intensity < 4) {
        /* Middle - normal color */
        printf("\033[0;%dm", 30 + color);
    } else {
        /* Tail - dim version */
        printf("\033[2;%dm", 30 + color);
    }

    /* Draw the shape using its coordinate data */
    for (int i = 0; i < MAX_BLOCK_LEN; i++) {
        int x = shape->rot_flat[0][i][0];
        int y = shape->rot_flat[0][i][1];

        /* Skip invalid coordinates */
        if (x < 0 || y < 0)
            continue;

        /* Single width for tight horizontal packing */
        int screen_x = base_x + x;
        int screen_y = base_y + y;

        /* Check bounds - use full terminal width for falling pieces effect */
        if (screen_x >= 0 && screen_x < ttcols - 1 && screen_y >= 0 &&
            screen_y < ttrows) {
            printf(ESC "[%d;%dH", screen_y + 1, screen_x + 1);
            printf("█"); /* Single-width block character for tight spacing */
        }
    }
}

/* Falling pieces effect for game over */
static void render_falling_pieces(const grid_t *g)
{
    static int piece_cols[FALLING_COLS];
    static int piece_speeds[FALLING_COLS];
    static int piece_shapes[FALLING_COLS]; /* Shape index for each column */
    static int piece_colors[FALLING_COLS]; /* Color for each column */
    static bool pieces_initialized = false;

    if (!pieces_initialized) {
        for (int i = 0; i < FALLING_COLS; i++) {
            piece_cols[i] = -(rand() % 20);     /* Stagger start times */
            piece_speeds[i] = 2 + (rand() % 4); /* Speed 2-5 (faster) */
            piece_shapes[i] = rand() % NUM_TETRIS_SHAPES; /* Random shape */
            piece_colors[i] =
                2 + (rand() % FALLING_COLORS); /* Random color 2-7 */
        }
        pieces_initialized = true;
    }

    for (int frame = 0; frame < 60;
         frame++) { /* Longer animation, more frames */
        /* Clear screen */
        printf(CLEAR_SCREEN);

        /* Update and draw falling pieces columns */
        for (int col = 0; col < FALLING_COLS; col++) {
            /* Very tight horizontal spacing - minimize gaps between columns */
            int x = col * (ttcols - 10) / FALLING_COLS + 2;

            /* Get the shape and color for this column */
            shape_t *shape = get_shape_by_index(piece_shapes[col]);
            if (!shape)
                continue;

            int shape_color = piece_colors[col];

            /* Draw falling shapes with increased vertical spacing for visual
             * appeal
             */
            for (int trail = 0; trail < 10; trail++) {
                /* Increased vertical spacing from 2 to 5 */
                int y = piece_cols[col] - trail * 5;
                if (y >= 0 && y < ttrows - 3)
                    draw_falling_shape(shape, x, y, shape_color, trail);
            }

            /* Update position */
            piece_cols[col] += piece_speeds[col];
            if (piece_cols[col] >
                ttrows + 25) { /* Account for increased spacing */
                piece_cols[col] = -(rand() % 15); /* Random restart position */
                piece_speeds[col] = 2 + (rand() % 4);
                /* New random shape */
                piece_shapes[col] = rand() % NUM_TETRIS_SHAPES;
                /* New random color */
                piece_colors[col] = 2 + (rand() % FALLING_COLORS);
            }
        }

        printf(COLOR_RESET);
        fflush(stdout);
        usleep(50000); /* Faster animation - 50ms delay (20fps) */
    }

    /* Clear screen after effect */
    printf(CLEAR_SCREEN);
    pieces_initialized = false;
}

/* Show falling pieces effect for game over */
void tui_show_falling_pieces(const grid_t *g)
{
    if (!g)
        return;

    render_falling_pieces(g);
}

/* Build display buffer: grid + current block + ghost */
static void build_display_buffer(const grid_t *g, block_t *falling_block)
{
    if (!g)
        return;

    /* 1. Clear buffer */
    for (int row = 0; row < GRID_HEIGHT; row++)
        for (int col = 0; col < GRID_WIDTH; col++)
            display_buffer[row][col] = 0;

    /* 2. Copy settled cells */
    for (int row = 0; row < g->height && row < GRID_HEIGHT; row++) {
        for (int col = 0; col < g->width && col < GRID_WIDTH; col++) {
            if (g->rows[row][col]) {
                int c = tui_get_block_color(col, row);
                display_buffer[row][col] = (c > 0) ? c : 2;
            }
        }
    }

    /* 3. Add ghost piece if falling block exists */
    if (falling_block && falling_block->shape) {
        int ghost_y = calculate_ghost_y(g, falling_block);

        /* Only draw ghost if it's different from current position */
        if (ghost_y != falling_block->offset.y) {
            block_t ghost_block = *falling_block;
            ghost_block.offset.y = ghost_y;

            for (int i = 0; i < MAX_BLOCK_LEN; i++) {
                coord_t cr;
                block_get(&ghost_block, i, &cr);

                /* Skip invalid entries */
                if (cr.x < 0 || cr.y < 0)
                    continue;

                /* Only draw ghost in empty cells */
                if (cr.x < GRID_WIDTH && cr.y < GRID_HEIGHT &&
                    display_buffer[cr.y][cr.x] == 0) {
                    display_buffer[cr.y][cr.x] = GHOST_COLOR;
                }
            }
        }
    }

    /* 4. Overlay the actual falling block (highest priority) */
    if (falling_block && falling_block->shape) {
        int fall_color = tui_get_shape_color(falling_block->shape);

        for (int i = 0; i < MAX_BLOCK_LEN; i++) {
            coord_t cr;
            block_get(falling_block, i, &cr);

            /* Ignore invalid entries */
            if (cr.x < 0 || cr.y < 0)
                continue;

            if (cr.x < GRID_WIDTH && cr.y < GRID_HEIGHT)
                display_buffer[cr.y][cr.x] = fall_color;
        }
    }

    display_buffer_valid = true;
}

/* Update sidebar statistics display */
static void update_sidebar_stats(void)
{
    int sidebar_x = GRID_WIDTH * 2 + 3;

    gotoxy(sidebar_x, 17);
    printf(COLOR_TEXT "Level  : %d      " COLOR_RESET, current_level);

    gotoxy(sidebar_x, 18);
    printf(COLOR_TEXT "Points : %d      " COLOR_RESET, current_points);

    gotoxy(sidebar_x, 19);
    printf(COLOR_TEXT "Lines  : %d      " COLOR_RESET, current_lines_cleared);
}

/* Update mode display */
static void update_sidebar_mode(void)
{
    int sidebar_x = GRID_WIDTH * 2 + 3;

    gotoxy(sidebar_x, 4);
    printf(COLOR_TEXT "Mode   : %-6s" COLOR_RESET,
           current_ai_mode ? "AI" : "Human");
}

/* Show static sidebar information and controls */
static void show_sidebar_info(void)
{
    int sidebar_x = GRID_WIDTH * 2 + 3;

    gotoxy(sidebar_x, 3);
    printf(COLOR_BORDER "TETRIS" COLOR_RESET);

    gotoxy(sidebar_x, 6);
    printf(COLOR_TEXT "space  : Toggle AI" COLOR_RESET);
    gotoxy(sidebar_x, 7);
    printf(COLOR_TEXT "p      : Pause" COLOR_RESET);
    gotoxy(sidebar_x, 8);
    printf(COLOR_TEXT "q      : Quit" COLOR_RESET);
    gotoxy(sidebar_x, 9);
    printf(COLOR_TEXT "arrows : Move / Rotate" COLOR_RESET);

    gotoxy(sidebar_x, 11);
    printf(COLOR_TEXT "Preview:" COLOR_RESET);

    update_sidebar_mode();
    update_sidebar_stats();
}

/* Full static frame (borders + sidebar) */
static void draw_static_frame(const grid_t *g)
{
    printf(HIDE_CURSOR CLEAR_SCREEN COLOR_BORDER);

    /* Top border */
    gotoxy(0, 0);
    printf("+");
    for (int col = 0; col < GRID_WIDTH * 2; col++)
        printf("-");
    printf("+");

    /* Side borders */
    int right = GRID_WIDTH * 2 + 1;
    for (int row = 1; row <= g->height; row++) {
        gotoxy(0, row);
        printf("|");
        gotoxy(right, row);
        printf("|");
    }

    /* Bottom border */
    gotoxy(0, g->height + 1);
    printf("+");
    for (int col = 0; col < GRID_WIDTH * 2; col++)
        printf("-");
    printf("+");

    printf(COLOR_RESET);
    show_sidebar_info();
}

void tui_setup(const grid_t *g)
{
    if (tty_size() < 0)
        return;

    enable_raw_mode();
    atexit(disable_raw_mode);
    printf(CLEAR_SCREEN HIDE_CURSOR);

    /* Reset auxiliary buffers */
    for (int y = 0; y < GRID_HEIGHT; y++) {
        for (int x = 0; x < GRID_WIDTH; x++) {
            shadow_board[y][x] = -1;
            color_grid[y][x] = 0;
            display_buffer[y][x] = 0;
        }
        dirty_row[y] = false; /* Initialize dirty row tracking */
    }
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++)
            shadow_preview[y][x] = 0;
    }

    /* Reset shape-to-color mapping */
    for (int i = 0; i < MAX_SHAPES; i++) {
        shape_colors[i].sig = 0;
        shape_colors[i].color = 0;
    }
    next_color = 2;

    /* Reset stats */
    current_level = 1;
    current_points = 0;
    current_lines_cleared = 0;
    current_ai_mode = false;

    display_buffer_valid = false;
    draw_static_frame(g);
}

/* Display buffer management */
void tui_build_display_buffer(const grid_t *g, block_t *falling_block)
{
    build_display_buffer(g, falling_block);

    /* Invalidate shadow buffer around falling block for clean updates */
    if (!falling_block || !falling_block->shape)
        return;

    for (int i = 0; i < MAX_BLOCK_LEN; i++) {
        coord_t cr;
        block_get(falling_block, i, &cr);
        if (cr.x < 0 || cr.x >= GRID_WIDTH || cr.y < 0 || cr.y >= GRID_HEIGHT)
            continue;

        shadow_board[cr.y][cr.x] = -1; /* Force redraw */

        /* Also invalidate surrounding area */
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                int nx = cr.x + dx, ny = cr.y + dy;
                if (nx >= 0 && nx < GRID_WIDTH && ny >= 0 && ny < GRID_HEIGHT)
                    shadow_board[ny][nx] = -1;
            }
        }
    }
}

void tui_render_display_buffer(const grid_t *g)
{
    if (!g || !display_buffer_valid)
        return;

    bool any_dirty = false;

    /* First pass: compare buffers and mark dirty rows */
    for (int row = 0; row < g->height && row < GRID_HEIGHT; row++) {
        dirty_row[row] = false;

        for (int col = 0; col < g->width && col < GRID_WIDTH; col++) {
            int color = display_buffer[row][col];

            /* Check if this cell changed */
            if (shadow_board[row][col] != color) {
                shadow_board[row][col] = color;
                dirty_row[row] = true; /* Mark this row as dirty */
            }
        }
        any_dirty |= dirty_row[row];
    }

    /* Early exit: nothing changed, skip all terminal I/O */
    if (!any_dirty)
        return;

    /* Second pass: draw only dirty rows */
    for (int row = 0; row < g->height && row < GRID_HEIGHT; row++) {
        if (!dirty_row[row])
            continue; /* Skip unchanged rows */

        /* Convert logical row to display row (invert Y-axis) */
        int display_y = g->height - row;

        /* Redraw all cells in this dirty row */
        for (int col = 0; col < g->width && col < GRID_WIDTH; col++) {
            int color = display_buffer[row][col];
            draw_block(col, display_y, color);
        }
    }

    fflush(stdout);
}

void tui_force_display_buffer_refresh(void)
{
    display_buffer_valid = false;

    /* Reset shadow buffer to force complete redraw */
    for (int y = 0; y < GRID_HEIGHT; y++) {
        for (int x = 0; x < GRID_WIDTH; x++) {
            shadow_board[y][x] = -1;
            display_buffer[y][x] = 0;
        }
        dirty_row[y] = true; /* Mark all rows dirty for complete refresh */
    }
}

/* Display preview of next tetromino piece */
void tui_block_print_preview(block_t *b, int color)
{
    int sidebar_x = GRID_WIDTH * 2 + 3;
    int preview_start_y = 12;

    /* Clear old preview area */
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 8; x++) {
            gotoxy(sidebar_x + x, preview_start_y + y);
            printf(" ");
        }
    }

    /* Clear shadow */
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            shadow_preview[y][x] = 0;

    /* Draw new preview if valid */
    if (b && b->shape && color > 0) {
        /* Ensure valid color */
        if (color < 2 || color > 7)
            color = 2;

        /* Create normalized preview block */
        block_t preview_copy = *b;
        preview_copy.offset.x = 0;
        preview_copy.offset.y = 0;
        preview_copy.rot = 0;

        for (int i = 0; i < MAX_BLOCK_LEN; i++) {
            coord_t cr;
            block_get(&preview_copy, i, &cr);

            if (cr.x < 0 || cr.y < 0 || cr.x >= 4 || cr.y >= 4)
                continue;

            /* Draw in preview area with centering */
            int preview_x = sidebar_x + (cr.x + 1) * 2;
            int preview_y = preview_start_y + cr.y + 1;

            if (preview_x >= sidebar_x && preview_x < sidebar_x + 8 &&
                preview_y >= preview_start_y &&
                preview_y < preview_start_y + 4) {
                gotoxy(preview_x, preview_y);
                bgcolor(color, "  ");
                printf(COLOR_RESET);
                shadow_preview[cr.y][cr.x] = color;
            }
        }
    }
    fflush(stdout);
}

/* Block color preservation */
void tui_add_block_color(block_t *b, int color)
{
    if (!b || !b->shape)
        return;

    /* Validate color */
    if (color < 2 || color > 7)
        color = 2;

    for (int i = 0; i < MAX_BLOCK_LEN; i++) {
        coord_t cr;
        block_get(b, i, &cr);

        if (cr.x < 0 || cr.y < 0)
            continue;

        if (cr.x < GRID_WIDTH && cr.y < GRID_HEIGHT)
            tui_set_block_color(cr.x, cr.y, color);
    }
}

/* Color preservation for line clearing */
void tui_prepare_color_preservation(const grid_t *g)
{
    /* Capture current color state before line clearing */
    for (int col = 0; col < GRID_WIDTH; col++) {
        preserved_counts[col] = 0;

        /* Collect colors bottom to top */
        for (int row = 0; row < g->height && row < GRID_HEIGHT; row++) {
            if (g->rows[row][col]) {
                int color = tui_get_block_color(col, row);
                if (color >= 2 && color <= 7 &&
                    preserved_counts[col] < GRID_HEIGHT)
                    preserved_colors[col][preserved_counts[col]++] = color;
            }
        }
    }
}

void tui_apply_color_preservation(const grid_t *g)
{
    /* Clear color grid */
    for (int y = 0; y < GRID_HEIGHT; y++)
        for (int x = 0; x < GRID_WIDTH; x++)
            color_grid[y][x] = 0;

    /* Reassign preserved colors */
    for (int col = 0; col < g->width && col < GRID_WIDTH; col++) {
        int color_index = 0;

        for (int row = 0; row < g->height; row++) {
            if (g->rows[row][col] && color_index < preserved_counts[col]) {
                color_grid[row][col] = preserved_colors[col][color_index++];
            } else if (g->rows[row][col]) {
                color_grid[row][col] = 2; /* Default */
            }
        }
    }

    tui_force_display_buffer_refresh();
}

/* Statistics updates */
void tui_update_stats(int level, int points, int lines_cleared)
{
    current_level = level;
    current_points = points;
    current_lines_cleared = lines_cleared;
    update_sidebar_stats();
    fflush(stdout);
}

/* Update mode display in sidebar */
void tui_update_mode_display(bool is_ai_mode)
{
    current_ai_mode = is_ai_mode;
    update_sidebar_mode();
    fflush(stdout);
}

/* Line clearing animation */
void tui_flash_completed_lines(const grid_t *g,
                               int *completed_rows,
                               int num_completed)
{
    if (num_completed <= 0)
        return;

    /* Flash effect */
    for (int flash = 0; flash < 3; flash++) {
        /* Flash bright white */
        for (int i = 0; i < num_completed; i++) {
            int row = completed_rows[i];
            for (int col = 0; col < g->width; col++) {
                int display_y = g->height - row;
                draw_block(col, display_y, 7); /* Bright white */
            }
        }
        fflush(stdout);
        usleep(100000); /* 0.1 second */

        /* Flash black */
        for (int i = 0; i < num_completed; i++) {
            int row = completed_rows[i];
            for (int col = 0; col < g->width; col++) {
                int display_y = g->height - row;
                draw_block(col, display_y, 0); /* Black */
            }
        }
        fflush(stdout);
        usleep(100000); /* 0.1 second */
    }
}

/* Force redraw functions */
void tui_force_redraw(const grid_t *g)
{
    /* Clear game area */
    int right_border_pos = GRID_WIDTH * 2 + 1;
    printf(COLOR_RESET);

    for (int y = 1; y <= GRID_HEIGHT; y++) {
        gotoxy(1, y);
        for (int x = 1; x < right_border_pos; x++)
            printf(" ");
    }

    tui_force_display_buffer_refresh();
    draw_static_frame(g);
    fflush(stdout);
}

/* Maintenance functions */
void tui_refresh_borders(const grid_t *g)
{
    printf(COLOR_BORDER);

    int border_width = g->width * 2;
    int right_border_pos = border_width + 1;

    /* Refresh borders */
    for (int i = 0; i <= g->height + 1; i++) {
        gotoxy(0, i);
        printf((i == 0 || i == g->height + 1) ? "+" : "|");

        gotoxy(right_border_pos, i);
        printf((i == 0 || i == g->height + 1) ? "+" : "|");
    }

    printf(COLOR_RESET);
}

void tui_periodic_cleanup(const grid_t *g)
{
    static int cleanup_counter = 0;
    cleanup_counter++;

    if (cleanup_counter % 300 == 0) {
        tui_refresh_borders(g);
        cleanup_counter = 0;
    }

    if (cleanup_counter % 100 == 0) {
        tui_force_display_buffer_refresh();
    }
}

/* Input and output */
void tui_prompt(const grid_t *g, const char *msg)
{
    if (!msg)
        return;

    gotoxy(g->width, g->height / 2 + 1);
    printf(COLOR_TEXT "%s" COLOR_RESET, msg);
    fflush(stdout);
}

void tui_refresh(void)
{
    fflush(stdout);
}

input_t tui_scankey(void)
{
    struct pollfd pfd = {.fd = STDIN_FILENO, .events = POLLIN, .revents = 0};

    /* Wait <= TUI_POLL_MS ms for input: keeps CPU cool yet feels instant */
    if ((poll(&pfd, 1, TUI_POLL_MS) > 0) && (pfd.revents & POLLIN)) {
        char c = getchar();
        switch (c) {
        case ' ':
            return INPUT_TOGGLE_MODE;
        case 'p':
        case 'P':
            return INPUT_PAUSE;
        case 'Q':
        case 'q':
            return INPUT_QUIT;
        case 27: /* ESC sequence for arrow keys */
            if (getchar() == '[') {
                c = getchar();
                switch (c) {
                case 'A': /* Up arrow */
                    return INPUT_ROTATE;
                case 'B': /* Down arrow */
                    return INPUT_DROP;
                case 'C': /* Right arrow */
                    return INPUT_MOVE_RIGHT;
                case 'D': /* Left arrow */
                    return INPUT_MOVE_LEFT;
                }
            }
            return INPUT_INVALID;
        default:
            return INPUT_INVALID;
        }
    }

    return INPUT_INVALID;
}

void tui_quit(void)
{
    printf(SHOW_CURSOR CLEAR_SCREEN);
    /* Move to absolute top-left corner */
    printf(ESC "[H");
    printf(COLOR_RESET);
    fflush(stdout);
    disable_raw_mode();
}

/* Grid printing using display buffer system */
void tui_grid_print(const grid_t *g)
{
    tui_build_display_buffer(g, NULL);
    tui_render_display_buffer(g);
}

/* Direct block printing for specific use cases */
void tui_block_print(block_t *b, int color, int grid_height)
{
    if (!b || !b->shape || color <= 0)
        return;

    if (color < 2 || color > 7)
        color = 2;

    for (int i = 0; i < MAX_BLOCK_LEN; i++) {
        coord_t cr;
        block_get(b, i, &cr);

        if (cr.x < 0 || cr.y < 0)
            continue;

        if (cr.x < GRID_WIDTH && cr.y < grid_height) {
            int display_y = grid_height - cr.y;
            draw_block(cr.x, display_y, color);

            if (cr.y < GRID_HEIGHT)
                shadow_board[cr.y][cr.x] = color;
        }
    }
    fflush(stdout);
}

/* Shadow block rendering */
void tui_block_print_shadow(block_t *b, int color, grid_t *g)
{
    if (!b || !b->shape || !g || color <= 0)
        return;

    if (color < 2 || color > 7)
        color = 2;

    int r = b->offset.y;
    grid_block_drop(g, b);
    tui_block_print(b, color, g->height);
    b->offset.y = r;
    tui_block_print(b, color, g->height);
}

/* Force grid redraw helper */
void tui_force_grid_redraw(void)
{
    for (int y = 0; y < GRID_HEIGHT; y++) {
        for (int x = 0; x < GRID_WIDTH; x++)
            shadow_board[y][x] = -1;
        dirty_row[y] = true; /* Mark all rows dirty for complete redraw */
    }
}
