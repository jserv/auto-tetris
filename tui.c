#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "tetris.h"

/* Alternative terminal buffer support */
#define ALT_BUF_ENABLE "\033[?1049h"
#define ALT_BUF_DISABLE "\033[?1049l"

/* Frame-based timing constants for 60fps consistency */
#define TUI_FRAME_MS 17        /* Frame duration for ~60fps */
#define TUI_INPUT_TIMEOUT_MS 1 /* Very short timeout for responsive input */
#define FRAME_TIME_US 16667    /* 16.67ms in microseconds */

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

/* Pre-built background-color sequences (index 0–7) */
static const char *bg_seq[8] = {
    "\033[0m",  "\033[0m",  "\033[42m", "\033[43m",
    "\033[44m", "\033[45m", "\033[46m", "\033[47m",
};
static const int bg_seq_len[8] = {4, 4, 5, 5, 5, 5, 5, 5};
#define GHOST_SEQ "\033[2;37m"
#define GHOST_SEQ_LEN 8

/* Ghost piece rendering */
#define GHOST_COLOR 9 /* Special sentinel for ghost piece */

/* Game over text pattern - readable "GAME OVER" in block letters
 * Pattern represents clear text for 14-column grid, 20 rows total
 * Since animation places lines from end of array first, "OVER" goes at start,
 * "GAME" at end
 */

/* clang-format off */
static const uint8_t GAME_OVER_TEXT_PATTERN[] = {
    /* Row 0-5: Empty space at top */
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,

    /* Row 6-11: "OVER" - O(3) V(3) E(3) R(3) = 12 cols + 2 spacing = 14 cols total */
    /* O V E R */
    1,1,1,0,2,0,0,3,3,3,0,4,0,0,  /* Row 6 (was Row 10) - R: bottom leg */
    1,0,1,0,2,0,0,3,0,0,0,4,0,4,  /* Row 7 (was Row 9) - R: diagonal */
    1,0,1,2,0,2,0,3,3,0,0,4,4,0,  /* Row 8 (was Row 8) - R: middle bar */
    1,0,1,2,0,2,0,3,0,0,0,4,0,4,  /* Row 9 (was Row 7) - R: vertical + right */
    1,1,1,2,0,2,0,3,3,3,0,4,4,4,  /* Row 10 (was Row 6) - R: top bar */
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,  /* Row 11 - spacing */

    /* Row 12-17: "GAME" - each letter 3 columns wide with spacing */
    /* G   A   M   E */
    5,5,5,0,6,0,6,0,1,0,1,0,2,2,  /* Row 12 (was Row 16) */
    5,0,6,0,6,0,6,0,1,0,1,0,2,0,  /* Row 13 (was Row 15) */
    5,0,6,0,6,6,6,0,1,1,1,0,2,2,  /* Row 14 (was Row 14) */
    5,0,0,0,6,0,6,0,1,1,1,0,2,0,  /* Row 15 (was Row 13) */
    5,5,5,0,0,6,0,0,1,0,1,0,2,2,  /* Row 16 (was Row 12) */
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,  /* Row 17 - spacing */

    /* Row 18-19: Empty space at bottom */
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0
};
/* clang-format on */

static const size_t GAME_OVER_TEXT_PATTERN_SIZE =
    sizeof(GAME_OVER_TEXT_PATTERN) / sizeof(GAME_OVER_TEXT_PATTERN[0]);

/* Layout constraints */
#define MIN_COLS 55
#define MIN_ROWS 21
#define MAX_SHAPES 7 /* I, J, L, O, S, T, Z – exactly seven */

/* Terminal state */
static struct termios orig_termios;
static int ttcols = 80, ttrows = 24; /* Terminal width and height */

/* Write-combining buffer for low-latency terminal output */
#define OUTBUF_SIZE 4096
#define FLUSH_THRESHOLD 2048 /* Flush when half-full for optimal latency */

static struct {
    char buf[OUTBUF_SIZE];
    size_t len;
    bool disabled; /* Emergency fallback when buffer management fails */
} outbuf = {0};

/* Fast cell access helper for TUI - matches grid.c implementation */
static inline bool tui_cell_occupied(const grid_t *g, int x, int y)
{
    return (g->rows[y] >> x) & 1ULL;
}

/* Helper function to handle write() return values safely */
static void safe_write(int fd, const void *buf, size_t count)
{
    ssize_t result = write(fd, buf, count);
    /* In terminal context, write failures are usually due to
     * broken pipes or terminal issues - not critical for game logic
     */
    if (result == -1)
        return;
}

/* Write-combining buffer management with automatic flushing */
static void outbuf_write(const char *data, size_t data_len)
{
    /* Emergency fallback: direct write if buffering disabled */
    if (outbuf.disabled) {
        safe_write(STDOUT_FILENO, data, data_len);
        return;
    }

    /* Handle writes larger than buffer */
    if (data_len >= OUTBUF_SIZE) {
        /* Flush current buffer first */
        if (outbuf.len > 0) {
            safe_write(STDOUT_FILENO, outbuf.buf, outbuf.len);
            outbuf.len = 0;
        }
        /* Write large data directly */
        safe_write(STDOUT_FILENO, data, data_len);
        return;
    }

    /* Check if this write would exceed buffer capacity */
    if (outbuf.len + data_len > OUTBUF_SIZE) {
        /* Flush current buffer to make room */
        safe_write(STDOUT_FILENO, outbuf.buf, outbuf.len);
        outbuf.len = 0;
    }

    /* Add data to buffer */
    memcpy(outbuf.buf + outbuf.len, data, data_len);
    outbuf.len += data_len;

    /* Auto-flush when reaching threshold for optimal latency */
    if (outbuf.len >= FLUSH_THRESHOLD) {
        safe_write(STDOUT_FILENO, outbuf.buf, outbuf.len);
        outbuf.len = 0;
    }
}

/* Formatted write to output buffer with bounds checking */
static void outbuf_printf(const char *format, ...)
{
    va_list args;
    va_start(args, format);

    /* Calculate space remaining in buffer */
    size_t remaining = OUTBUF_SIZE - outbuf.len;

    /* Try to format into remaining buffer space */
    int written = vsnprintf(outbuf.buf + outbuf.len, remaining, format, args);
    va_end(args);

    if (written < 0) {
        /* Formatting error - use emergency fallback */
        outbuf.disabled = true;
        return;
    }

    if ((size_t) written >= remaining) {
        /* Output was truncated - flush buffer and retry */
        if (outbuf.len > 0) {
            safe_write(STDOUT_FILENO, outbuf.buf, outbuf.len);
            outbuf.len = 0;
        }

        /* Retry formatting with full buffer */
        va_start(args, format);
        written = vsnprintf(outbuf.buf, OUTBUF_SIZE, format, args);
        va_end(args);

        if (written < 0 || (size_t) written >= OUTBUF_SIZE) {
            /* Still doesn't fit or error - use emergency fallback */
            outbuf.disabled = true;
            return;
        }
    }

    /* Update buffer length */
    outbuf.len += written;

    /* Auto-flush when reaching threshold */
    if (outbuf.len >= FLUSH_THRESHOLD) {
        safe_write(STDOUT_FILENO, outbuf.buf, outbuf.len);
        outbuf.len = 0;
    }
}

/* Force flush of output buffer */
static void outbuf_flush(void)
{
    if (outbuf.len > 0) {
        safe_write(STDOUT_FILENO, outbuf.buf, outbuf.len);
        outbuf.len = 0;
    }
    /* Keep stdio in sync for any legacy printf() calls */
    fflush(stdout);
}

/* Batched cell structure */
typedef struct {
    int x, y;
    int color;
    char symbol[8]; /* Room for "  " and Unicode characters */
} render_cell_t;

/* frame-local buffer (max board is 14×20 → 280 plus UI chrome) */
#define MAX_BATCH 512
static render_cell_t batch[MAX_BATCH];
static size_t batch_count = 0;

static inline int by_pos(const void *a, const void *b)
{
    const render_cell_t *A = a, *B = b;
    if (A->y != B->y)
        return A->y - B->y; /* Sort by row first (top to bottom) */
    return A->x - B->x;     /* Then by column (left to right) */
}

/* Push a cell into the batch (no realloc for tiny board). */
static void push_cell(int x, int y, int color, const char *symbol)
{
    if (batch_count >= MAX_BATCH)
        return; /* silent overflow guard */
    render_cell_t *c = &batch[batch_count++];
    c->x = x;
    c->y = y;
    c->color = color;
    strncpy(c->symbol, symbol, sizeof(c->symbol) - 1);
    c->symbol[sizeof(c->symbol) - 1] = 0;
}

/* Flush batch: minimize cursor + colour changes, one write per line. */
static void tui_batch_flush(void)
{
    if (!batch_count) {
        outbuf_flush();
        return;
    }

    qsort(batch, batch_count, sizeof(batch[0]), by_pos);

    int cur_y = -1, cur_x = -1, cur_color = -1;

    /* Calculate screen offset for centering (same as gotoxy) */
    int xpos = (ttcols - MIN_COLS) / 2;
    int ypos = (ttrows - MIN_ROWS) / 2;

    for (size_t i = 0; i < batch_count;) {
        render_cell_t *c = &batch[i];

        /* Detect a run of cells that sit on the same row, share the same
         * color, and are laid out contiguously (each board cell is two
         * characters wide).
         */
        size_t run = 1;
        while (i + run < batch_count) {
            render_cell_t *n = &batch[i + run];
            if (n->y != c->y || n->color != c->color ||
                n->x != c->x + (int) run * 2)
                break;
            run++;
        }

        /* Convert relative coordinates to absolute screen coordinates */
        int screen_x = xpos + c->x;
        int screen_y = ypos + c->y;

        /* Move cursor if position changed significantly */
        if (c->y != cur_y || c->x != cur_x) {
            outbuf_printf("\x1b[%d;%dH", screen_y, screen_x);
            cur_color = -1; /* Force color reset after cursor move */
        }

        /* Color change with complete reset for clean state */
        if (c->color != cur_color) {
            outbuf_write("\033[0m", 4); /* full reset */

            if (c->color == GHOST_COLOR) /* dim white */
                outbuf_write(GHOST_SEQ, GHOST_SEQ_LEN);
            else if (c->color >= 2 && c->color <= 7) /* bg 42–47 */
                outbuf_write(bg_seq[c->color], bg_seq_len[c->color]);
            cur_color = c->color;
        }

        /* Emit the entire run in one go – fewer cursor checks, fewer writes. */
        for (size_t k = 0; k < run; k++)
            outbuf_write((c + k)->symbol, strlen((c + k)->symbol));

        cur_y = c->y;
        cur_x = c->x + (int) run * 2; /* advance cursor past the run */

        i += run; /* Skip the cells we just rendered */
    }

    /* Ensure clean color state after batch rendering */
    outbuf_write("\033[0m", 4); /* Complete reset */
    outbuf_flush();
    batch_count = 0;
}

/* Off-screen buffers */
static int shadow_board[GRID_HEIGHT][GRID_WIDTH];
static int shadow_preview[4][4];

/* Color for every settled grid cell */
static int color_grid[GRID_HEIGHT][GRID_WIDTH];

/* Double-buffer to draw only what changed */
static int display_buffer[GRID_HEIGHT][GRID_WIDTH];
static bool buffer_valid = false;

/* Row-level dirty tracking for optimized rendering */
static bool dirty_row[GRID_HEIGHT];

/* Direct-indexed color cache for O(1) shape color lookup */
static struct {
    unsigned sig;                 /* Geometry signature (unique per shape) */
    int color;                    /* Value: ANSI background color 2–7 */
} shape_colors[MAX_SHAPES] = {0}; /* Fixed slots for signature-indexed access */

static int next_color = 2; /* Round-robin allocator: 2 → 7 → 2 … */

/* Game statistics for sidebar display */
static int level = 1;
static int points = 0;
static int lines = 0;
static bool ai_mode = false;

/* Color preservation for line clearing */
static int preserved_colors[GRID_WIDTH][GRID_HEIGHT];
static int preserved_counts[GRID_WIDTH];

/* Return the persistent color for a shape */
int tui_get_shape_color(const shape_t *shape)
{
    if (!shape)
        return 2; /* Visible default */

    unsigned sig = shape->sig;
    int index = sig % MAX_SHAPES; /* Direct index calculation */

    /* Fast path: cache hit with matching signature */
    if (shape_colors[index].sig == sig)
        return shape_colors[index].color;

    /* Cache miss or collision: assign new color */
    int assigned_color = next_color;
    next_color = (next_color == 6) ? 2 : next_color + 1; /* Cycle 2-6 */

    /* Store in indexed slot (LRU-style replacement) */
    shape_colors[index].sig = sig;
    shape_colors[index].color = assigned_color;

    return assigned_color;
}

/* Raw-mode helpers */
static void disable_raw(void)
{
    /* Flush any pending output first */
    outbuf_flush();

    /* Return to the main buffer */
    safe_write(STDOUT_FILENO, ALT_BUF_DISABLE, sizeof(ALT_BUF_DISABLE) - 1);

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

static void enable_raw(void)
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
static int get_tty_size(void)
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

    outbuf_printf("\033[%d;%dH", ypos + y, xpos + x);
}

static void draw_block(int x, int y, int color)
{
    /* Out-of-bounds? Don't draw. */
    if (x < 0 || x >= GRID_WIDTH || y < 1 || y > GRID_HEIGHT)
        return;

    int draw_x = x * 2 + 1;
    int right_border_pos = GRID_WIDTH * 2 + 1;

    if (draw_x >= 1 && draw_x + 1 < right_border_pos) {
        /* Clamp color – any invalid value becomes empty space (color 0) */
        if (color < 0 || color > 9)
            color = 0;

        if (color == GHOST_COLOR) {
            push_cell(draw_x, y, GHOST_COLOR, "░░");
        } else if (color == 0) {
            push_cell(draw_x, y, 0, "  ");
        } else {
            push_cell(draw_x, y, color, "  ");
        }
    }
}

/* Render ghost piece */
static void render_ghost(const grid_t *g, const block_t *falling_block)
{
    if (!g || !falling_block || !falling_block->shape)
        return;

    /* Create a copy of the falling block for ghost calculation */
    block_t ghost_block = *falling_block;

    /* Find final position */
    grid_block_drop(g, &ghost_block);

    /* Only render ghost if it's different from current position */
    if (ghost_block.offset.y == falling_block->offset.y)
        return;

    /* Render ghost piece in display buffer */
    for (int i = 0; i < MAX_BLOCK_LEN; i++) {
        coord_t cr;
        block_get(&ghost_block, i, &cr);

        /* Skip invalid coordinates */
        if (cr.x < 0 || cr.y < 0)
            continue;

        /* Only draw ghost in empty cells within bounds */
        if (cr.x < GRID_WIDTH && cr.y < GRID_HEIGHT &&
            display_buffer[cr.y][cr.x] == 0) {
            display_buffer[cr.y][cr.x] = GHOST_COLOR;
        }
    }
}

/* Color-grid helpers (settled blocks) */
static void set_block_color(int x, int y, int color)
{
    if (x >= 0 && x < GRID_WIDTH && y >= 0 && y < GRID_HEIGHT) {
        if (color < 2 || color > 7)
            color = 2;
        color_grid[y][x] = color;
    }
}

static int tui_get_block_color(int x, int y)
{
    if (x >= 0 && x < GRID_WIDTH && y >= 0 && y < GRID_HEIGHT)
        return color_grid[y][x];
    return 0;
}

/* Get color for shape index (matching auto-tetris color scheme) */
static int get_shape_color(int shape_index)
{
    /* Map shape indices 0-6 to colors 2-7, with 0 = empty */
    if (shape_index <= 0)
        return 0; /* Empty cell */
    if (shape_index > 6)
        return 2;           /* Fallback color */
    return shape_index + 1; /* Map 1-6 to colors 2-7 */
}

/* Helper function to populate color grid from pattern data - ttytris style */
static void tui_set_pattern_data(const uint8_t *pattern_data,
                                 size_t data_size,
                                 size_t offset)
{
    if (offset + data_size > GRID_HEIGHT * GRID_WIDTH)
        return;

    for (size_t i_p = offset, i_c = 0; i_c < data_size; ++i_c, ++i_p) {
        int x = i_p % GRID_WIDTH;
        int y = i_p / GRID_WIDTH;

        if (x < GRID_WIDTH && y < GRID_HEIGHT) {
            int shape_index = pattern_data[i_c];
            int color = get_shape_color(shape_index);
            color_grid[y][x] = color;
        }
    }
}

/* Grid dissolution animation - elegant disappearing effect */
static void render_grid_dissolution(const grid_t *g)
{
    if (!g)
        return;

    /* Animation timing - slightly faster than text reveal */
    const int FRAME_DELAY_US = 50000; /* 50ms per phase */

    /* Copy current grid state to color_grid for dissolution */
    for (int y = 0; y < g->height && y < GRID_HEIGHT; y++) {
        for (int x = 0; x < g->width && x < GRID_WIDTH; x++) {
            uint64_t row = g->rows[g->height - 1 - y];
            bool occupied = (row >> x) & 1;

            if (occupied) {
                /* Use existing color or default if not set */
                if (color_grid[y][x] == 0)
                    color_grid[y][x] = 2; /* Default color for occupied cells */
            } else {
                color_grid[y][x] = 0;
            }
        }
    }

    /* Dissolution effect: fade cells row by row from bottom to top */
    for (int dissolve_row = 0; dissolve_row < g->height; dissolve_row++) {
        /* Clear this row */
        for (int x = 0; x < g->width && x < GRID_WIDTH; x++) {
            color_grid[dissolve_row][x] = 0;
        }

        /* Render current state */
        for (int y = 1; y <= g->height; y++) {
            for (int x = 0; x < g->width && x < GRID_WIDTH; x++) {
                int grid_y = g->height - y;
                int color = color_grid[grid_y][x];
                draw_block(x, y, color);
            }
        }

        tui_batch_flush();
        usleep(FRAME_DELAY_US);
    }

    /* Ensure grid is completely clear */
    for (int y = 0; y < GRID_HEIGHT; y++) {
        for (int x = 0; x < GRID_WIDTH; x++)
            color_grid[y][x] = 0;
    }
}

/* Game over animation - bottom-to-top text reveal */
static void render_game_over_text(const grid_t *g)
{
    if (!g)
        return;

    /* Animation timing - 75ms per line reveal */
    const int FRAME_DELAY_US = 75000;

    /* Calculate pattern dimensions */
    const int game_over_lines = GAME_OVER_TEXT_PATTERN_SIZE / g->width;

    /* Clear the grid area first */
    for (int y = 0; y < GRID_HEIGHT; y++) {
        for (int x = 0; x < GRID_WIDTH; x++)
            color_grid[y][x] = 0;
    }

    /* Animate line-by-line reveal from bottom to top */
    for (int i = 0; i < game_over_lines; i++) {
        /* Clear the top line to make room for new pattern line */
        for (int y = GRID_HEIGHT - 1; y > 0; y--) {
            for (int x = 0; x < GRID_WIDTH; x++)
                color_grid[y][x] = color_grid[y - 1][x];
        }
        for (int x = 0; x < GRID_WIDTH; x++)
            color_grid[0][x] = 0;

        /* Set the new bottom line from pattern data */
        int pattern_line_start = (game_over_lines - i - 1) * g->width;
        tui_set_pattern_data(&GAME_OVER_TEXT_PATTERN[pattern_line_start],
                             g->width, 0);

        /* Render the current state */
        for (int y = 1; y <= g->height; y++) {
            for (int x = 0; x < g->width && x < GRID_WIDTH; x++) {
                int grid_y = g->height - y;
                int color = color_grid[grid_y][x];
                draw_block(x, y, color);
            }
        }

        tui_batch_flush();
        usleep(FRAME_DELAY_US);
    }

    /* Hold the final text briefly */
    usleep(FRAME_DELAY_US * 2);
}

/* Show game over animation sequence: dissolution first, then text */
void tui_animate_gameover(const grid_t *g)
{
    if (!g)
        return;

    /* First: show elegant grid dissolution effect */
    render_grid_dissolution(g);

    /* Brief pause between animations */
    usleep(100000); /* 100ms pause */

    /* Second: show "GAME OVER" text reveal */
    render_game_over_text(g);
}

/* Build display buffer: grid + current block + ghost */
static void build_buffer(const grid_t *g, const block_t *falling_block)
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
            if (tui_cell_occupied(g, col, row)) {
                int c = tui_get_block_color(col, row);
                display_buffer[row][col] = (c > 0) ? c : 2;
            }
        }
    }

    /* 3. Add enhanced ghost piece if falling block exists */
    if (falling_block && falling_block->shape)
        render_ghost(g, falling_block);

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

    buffer_valid = true;
}

/* Update sidebar statistics display */
static void update_stats(void)
{
    int sidebar_x = GRID_WIDTH * 2 + 3;

    gotoxy(sidebar_x, 17);
    outbuf_printf(COLOR_TEXT "Level  : %d      " COLOR_RESET, level);

    gotoxy(sidebar_x, 18);
    outbuf_printf(COLOR_TEXT "Points : %d      " COLOR_RESET, points);

    gotoxy(sidebar_x, 19);
    outbuf_printf(COLOR_TEXT "Lines  : %d      " COLOR_RESET, lines);

    /* Ensure clean state after stats */
    outbuf_write(COLOR_RESET, strlen(COLOR_RESET));
}

/* Update mode display */
static void update_mode(void)
{
    int sidebar_x = GRID_WIDTH * 2 + 3;

    gotoxy(sidebar_x, 4);
    outbuf_printf(COLOR_TEXT "Mode   : %-6s" COLOR_RESET,
                  ai_mode ? "AI" : "Human");

    /* Ensure clean state after mode display */
    outbuf_write(COLOR_RESET, strlen(COLOR_RESET));
}

/* Show static sidebar information and controls */
static void show_sidebar(void)
{
    int sidebar_x = GRID_WIDTH * 2 + 3;

    /* Ensure clean state for sidebar */
    outbuf_write(COLOR_RESET, strlen(COLOR_RESET));

    gotoxy(sidebar_x, 3);
    outbuf_write(COLOR_BORDER "TETRIS" COLOR_RESET,
                 strlen(COLOR_BORDER "TETRIS" COLOR_RESET));

    gotoxy(sidebar_x, 6);
    outbuf_write(COLOR_TEXT "space  : Toggle AI" COLOR_RESET,
                 strlen(COLOR_TEXT "space  : Toggle AI" COLOR_RESET));
    gotoxy(sidebar_x, 7);
    outbuf_write(COLOR_TEXT "p      : Pause" COLOR_RESET,
                 strlen(COLOR_TEXT "p      : Pause" COLOR_RESET));
    gotoxy(sidebar_x, 8);
    outbuf_write(COLOR_TEXT "q      : Quit" COLOR_RESET,
                 strlen(COLOR_TEXT "q      : Quit" COLOR_RESET));
    gotoxy(sidebar_x, 9);
    outbuf_write(COLOR_TEXT "arrows : Move / Rotate" COLOR_RESET,
                 strlen(COLOR_TEXT "arrows : Move / Rotate" COLOR_RESET));

    gotoxy(sidebar_x, 11);
    outbuf_write(COLOR_TEXT "Preview:" COLOR_RESET,
                 strlen(COLOR_TEXT "Preview:" COLOR_RESET));

    update_mode();
    update_stats();

    /* Ensure clean state after sidebar */
    outbuf_write(COLOR_RESET, strlen(COLOR_RESET));
}

/* Full static frame (borders + sidebar) */
static void draw_frame(const grid_t *g)
{
    /* Complete flush of any pending batch operations first */
    tui_batch_flush();

    /* Draw borders using direct buffered output (bypass batch renderer) */
    outbuf_write(HIDE_CURSOR CLEAR_SCREEN, strlen(HIDE_CURSOR CLEAR_SCREEN));

    /* Apply border color directly */
    outbuf_write(COLOR_BORDER, strlen(COLOR_BORDER));

    /* Top border */
    gotoxy(0, 0);
    outbuf_write("+", 1);
    for (int col = 0; col < GRID_WIDTH * 2; col++)
        outbuf_write("-", 1);
    outbuf_write("+", 1);

    /* Side borders */
    int right = GRID_WIDTH * 2 + 1;
    for (int row = 1; row <= g->height; row++) {
        gotoxy(0, row);
        outbuf_write("|", 1);
        gotoxy(right, row);
        outbuf_write("|", 1);
    }

    /* Bottom border */
    gotoxy(0, g->height + 1);
    outbuf_write("+", 1);
    for (int col = 0; col < GRID_WIDTH * 2; col++)
        outbuf_write("-", 1);
    outbuf_write("+", 1);

    /* Reset color and flush borders immediately */
    outbuf_write(COLOR_RESET, strlen(COLOR_RESET));
    outbuf_flush(); /* Immediate flush to ensure borders are drawn */

    show_sidebar();
}

void tui_setup(const grid_t *g)
{
    if (get_tty_size() < 0)
        return;

    /* Switch to alternative buffer before setup */
    outbuf_write(ALT_BUF_ENABLE, strlen(ALT_BUF_ENABLE));

    enable_raw();
    atexit(disable_raw);

    /* Use buffered output for initial setup */
    outbuf_write(CLEAR_SCREEN HIDE_CURSOR, strlen(CLEAR_SCREEN HIDE_CURSOR));
    outbuf_flush(); /* Immediate flush for setup */

    /* Reset auxiliary buffers */
    for (int y = 0; y < GRID_HEIGHT; y++) {
        for (int x = 0; x < GRID_WIDTH; x++) {
            /* Use distinctive value to force initial redraw */
            shadow_board[y][x] = -999;
            color_grid[y][x] = 0;
            display_buffer[y][x] = 0;
        }
        dirty_row[y] = true; /* Initialize all rows as dirty */
    }
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++)
            shadow_preview[y][x] = 0;
    }

    /* Reset shape-to-color mapping cache */
    for (int i = 0; i < MAX_SHAPES; i++) {
        shape_colors[i].sig = 0;
        shape_colors[i].color = 0;
    }
    next_color = 2;

    /* Reset stats */
    level = 1;
    points = 0;
    lines = 0;
    ai_mode = false;

    buffer_valid = false;
    draw_frame(g);
    outbuf_flush(); /* Ensure frame is displayed before continuing */
}

/* Display buffer management */
void tui_build_buffer(const grid_t *g, const block_t *falling_block)
{
    build_buffer(g, falling_block);

    /* Invalidate shadow buffer around falling block for clean updates */
    if (!falling_block || !falling_block->shape)
        return;

    for (int i = 0; i < MAX_BLOCK_LEN; i++) {
        coord_t cr;
        block_get(falling_block, i, &cr);
        if (cr.x < 0 || cr.x >= GRID_WIDTH || cr.y < 0 || cr.y >= GRID_HEIGHT)
            continue;

        shadow_board[cr.y][cr.x] = -999; /* Force redraw */

        /* Also invalidate surrounding area */
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                int nx = cr.x + dx, ny = cr.y + dy;
                if (nx >= 0 && nx < GRID_WIDTH && ny >= 0 && ny < GRID_HEIGHT)
                    shadow_board[ny][nx] = -999;
            }
        }
    }
}

void tui_render_buffer(const grid_t *g)
{
    if (!g || !buffer_valid)
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
    if (!any_dirty) {
        outbuf_flush();
        return;
    }

    /* Second pass: draw only dirty rows using batch renderer */
    for (int row = 0; row < g->height && row < GRID_HEIGHT; row++) {
        if (!dirty_row[row])
            continue; /* Skip unchanged rows */

        /* Convert logical row to display row (invert Y-axis) */
        int display_y = g->height - row;

        /* Add all cells in this dirty row to batch */
        for (int col = 0; col < g->width && col < GRID_WIDTH; col++) {
            int color = display_buffer[row][col];
            draw_block(col, display_y, color);
        }
    }

    /* Flush batch - this handles all color management internally */
    tui_batch_flush();
}

void tui_refresh_force(void)
{
    buffer_valid = false;

    /* Reset shadow buffer to force complete redraw */
    for (int y = 0; y < GRID_HEIGHT; y++) {
        for (int x = 0; x < GRID_WIDTH; x++) {
            /* Use distinctive value to force redraw */
            shadow_board[y][x] = -999;
            display_buffer[y][x] = 0;
        }
        dirty_row[y] = true; /* Mark all rows dirty for complete refresh */
    }
}

/* Display preview of next tetromino piece */
void tui_show_preview(const block_t *b, int color)
{
    int sidebar_x = GRID_WIDTH * 2 + 3;
    int preview_start_y = 12;

    /* Clear old preview area */
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 10; x++) {
            gotoxy(sidebar_x + x, preview_start_y + y);
            outbuf_write(" ", 1);
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

        /* Get shape height for proper centering */
        int shape_height = preview_copy.shape->rot_wh[0].y;

        for (int i = 0; i < MAX_BLOCK_LEN; i++) {
            coord_t cr;
            block_get(&preview_copy, i, &cr);

            if (cr.x < 0 || cr.y < 0 || cr.x >= 4 || cr.y >= 4)
                continue;

            /* Draw in preview area with centering */
            int preview_x = sidebar_x + (cr.x + 1) * 2;
            /* Flip Y coordinate to match main grid orientation, centered in
             * preview area
             */
            int flipped_y = (shape_height - 1) - cr.y;
            int preview_y = preview_start_y + flipped_y + 1;

            if (preview_x >= sidebar_x && preview_x < sidebar_x + 10 &&
                preview_y >= preview_start_y &&
                preview_y < preview_start_y + 4) {
                gotoxy(preview_x, preview_y);
                /* Use pre-built color sequences for consistent performance */
                if (color >= 2 && color <= 7) {
                    outbuf_write(bg_seq[color], bg_seq_len[color]);
                    outbuf_write("  " COLOR_RESET, 6);
                } else {
                    outbuf_write("  ", 2);
                }
                shadow_preview[flipped_y][cr.x] = color;
            }
        }
    }
    outbuf_flush();
}

/* Block color preservation */
void tui_add_block_color(const block_t *b, int color)
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
            set_block_color(cr.x, cr.y, color);
    }
}

/* Color preservation for line clearing */
void tui_save_colors(const grid_t *g)
{
    /* Capture current color state before line clearing */
    for (int col = 0; col < GRID_WIDTH; col++) {
        preserved_counts[col] = 0;

        /* Collect colors bottom to top */
        for (int row = 0; row < g->height && row < GRID_HEIGHT; row++) {
            if (tui_cell_occupied(g, col, row)) {
                int color = tui_get_block_color(col, row);
                if (color >= 2 && color <= 7 &&
                    preserved_counts[col] < GRID_HEIGHT)
                    preserved_colors[col][preserved_counts[col]++] = color;
            }
        }
    }
}

void tui_restore_colors(const grid_t *g)
{
    /* Clear color grid */
    for (int y = 0; y < GRID_HEIGHT; y++)
        for (int x = 0; x < GRID_WIDTH; x++)
            color_grid[y][x] = 0;

    /* Reassign preserved colors */
    for (int col = 0; col < g->width && col < GRID_WIDTH; col++) {
        int color_index = 0;

        for (int row = 0; row < g->height; row++) {
            if (tui_cell_occupied(g, col, row) &&
                color_index < preserved_counts[col]) {
                color_grid[row][col] = preserved_colors[col][color_index++];
            } else if (tui_cell_occupied(g, col, row)) {
                color_grid[row][col] = 2; /* Default */
            }
        }
    }

    tui_refresh_force();
}

/* Statistics updates */
void tui_update_stats(int new_level, int new_points, int new_lines)
{
    level = new_level;
    points = new_points;
    lines = new_lines;
    update_stats();
    outbuf_flush();
}

/* Update mode display in sidebar */
void tui_update_mode_display(bool is_ai_mode)
{
    ai_mode = is_ai_mode;
    update_mode();
    outbuf_flush();
}

/* Frame-based line clearing animation with consistent timing */
void tui_flash_lines(const grid_t *g,
                     const int *completed_rows,
                     int num_completed)
{
    if (num_completed <= 0)
        return;

    /* Ensure clean state before animation */
    outbuf_flush();

    /* Frame-based inward clearing animation with 5 phases */
    const int PHASE_DURATION_US = 83333; /* 5 frames at 60fps */

    for (int phase = 0; phase < 5; phase++) {
        for (int i = 0; i < num_completed; i++) {
            int row = completed_rows[i];
            int display_y = g->height - row;

            /* Clear from edges inward */
            for (int col = 0; col < g->width; col++) {
                if (col >= phase && col < g->width - phase) {
                    /* Still-to-be-cleared cells flash white */
                    draw_block(col, display_y, 7);
                } else {
                    /* Already cleared cells become empty */
                    draw_block(col, display_y, 0);
                }
            }
        }
        tui_batch_flush();

        /* Simple fixed timing: 83ms per phase */
        usleep(PHASE_DURATION_US);
    }

    /* Final phase: all cells cleared */
    for (int i = 0; i < num_completed; i++) {
        int row = completed_rows[i];
        int display_y = g->height - row;
        for (int col = 0; col < g->width; col++)
            draw_block(col, display_y, 0);
    }
    tui_batch_flush();

    /* Brief pause before game continues - 100ms */
    usleep(100000);
}

/* Force redraw functions */
void tui_force_redraw(const grid_t *g)
{
    /* Clear game area */
    int right_border_pos = GRID_WIDTH * 2 + 1;
    outbuf_write(COLOR_RESET, strlen(COLOR_RESET));

    for (int y = 1; y <= GRID_HEIGHT; y++) {
        gotoxy(1, y);
        for (int x = 1; x < right_border_pos; x++)
            outbuf_write(" ", 1);
    }

    tui_refresh_force();
    draw_frame(g);
    outbuf_flush();
}

/* Maintenance functions */
void tui_refresh_borders(const grid_t *g)
{
    /* Complete flush of any pending batch operations first */
    tui_batch_flush();

    /* Apply border color directly */
    outbuf_write(COLOR_BORDER, strlen(COLOR_BORDER));

    int border_width = g->width * 2;
    int right_border_pos = border_width + 1;

    /* Refresh borders */
    for (int i = 0; i <= g->height + 1; i++) {
        gotoxy(0, i);
        outbuf_write((i == 0 || i == g->height + 1) ? "+" : "|", 1);

        gotoxy(right_border_pos, i);
        outbuf_write((i == 0 || i == g->height + 1) ? "+" : "|", 1);
    }

    /* Reset color and flush borders immediately */
    outbuf_write(COLOR_RESET, strlen(COLOR_RESET));
    outbuf_flush(); /* Immediate flush to ensure borders are drawn */
}

void tui_cleanup_display(const grid_t *g)
{
    static int cleanup_counter = 0;
    cleanup_counter++;

    if (cleanup_counter % 300 == 0) {
        tui_refresh_borders(g);
        cleanup_counter = 0;
    }

    if (cleanup_counter % 100 == 0)
        tui_refresh_force();
}

/* Input and output */
void tui_prompt(const grid_t *g, const char *msg)
{
    if (!msg)
        return;

    gotoxy(g->width, g->height / 2 + 1);
    outbuf_printf(COLOR_TEXT "%s" COLOR_RESET, msg);
    outbuf_flush();
}

void tui_refresh(void)
{
    outbuf_flush();
}

input_t tui_scankey(void)
{
    struct pollfd pfd = {.fd = STDIN_FILENO, .events = POLLIN, .revents = 0};

    /* Frame-based input polling with very short timeout for responsiveness */
    if ((poll(&pfd, 1, TUI_INPUT_TIMEOUT_MS) > 0) && (pfd.revents & POLLIN)) {
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
    outbuf_flush();

    /* ALT‑BUF already disabled above; just restore cursor and clear. */
    safe_write(STDOUT_FILENO, SHOW_CURSOR CLEAR_SCREEN,
               sizeof(SHOW_CURSOR CLEAR_SCREEN) - 1);
    safe_write(STDOUT_FILENO, ESC "[H",
               sizeof(ESC "[H") - 1); /* Move to absolute top-left corner */
    safe_write(STDOUT_FILENO, COLOR_RESET, sizeof(COLOR_RESET) - 1);

    disable_raw();
}
