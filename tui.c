#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

#include "tetris.h"

/* ANSI escape sequences */
#define ESC "\033"
#define CLEAR_SCREEN ESC "[2J" ESC "[1;1H"
#define HIDE_CURSOR ESC "[?25l"
#define SHOW_CURSOR ESC "[?25h"
#define RESET_COLOR ESC "[0m"

/* Colors */
#define bgcolor(c, s) printf("\033[%dm" s, c ? c + 40 : 0)
#define COLOR_RESET ESC "[0m"
#define COLOR_BORDER ESC "[1;32m" /* Bright green text for border */
#define COLOR_TEXT ESC "[0;37m"   /* White text */

/* Layout constants */
#define MIN_COLS 55
#define MIN_ROWS 21

static struct termios orig_termios;
static int ttcols = 80; /* terminal width */
static int ttrows = 24; /* terminal height */

/* Shadow buffer for efficient updates */
static int shadow_board[GRID_HEIGHT][GRID_WIDTH];
static int shadow_preview[4][4];

/* Store colors of placed blocks */
static int color_grid[GRID_HEIGHT][GRID_WIDTH];

/* Display buffer for double-buffering approach */
int display_buffer[GRID_HEIGHT][GRID_WIDTH];
static bool display_buffer_valid = false;

static void gotoxy(int x, int y)
{
    int xpos = (ttcols - MIN_COLS) / 2;
    int ypos = (ttrows - MIN_ROWS) / 2;

    printf(ESC "[%d;%dH", ypos + y, xpos + x);
}

static void draw_block(int x, int y, int color)
{
    /* bounds checking to prevent any clutter */
    if (x < 0 || x >= GRID_WIDTH || y < 1 || y > GRID_HEIGHT)
        return; /* Out of bounds - don't draw */

    /* Calculate drawing position with border offset */
    int draw_x = x * 2 + 1;
    int right_border_pos = GRID_WIDTH * 2 + 1;

    /* Ensure we don't overwrite borders and stay within bounds */
    if (draw_x >= 1 && draw_x + 1 < right_border_pos) {
        gotoxy(draw_x, y);
        if (color == 0) {
            /* Empty space - draw spaces to clear */
            printf("  ");
        } else {
            /* Colored block - ensure proper color reset */
            bgcolor(color, "  ");
        }
        printf(COLOR_RESET);
    }
}

static int tty_size(void)
{
    struct winsize ws = {0};

    if (!ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws)) {
        ttcols = ws.ws_col;
        ttrows = ws.ws_row;
    }

    if (ttcols < MIN_COLS || ttrows < MIN_ROWS) {
        fprintf(stderr, "Terminal too small (min: %dx%d)\n", MIN_COLS,
                MIN_ROWS);
        return -1;
    }

    return 0;
}

/* Save original terminal settings and set raw mode */
static void enable_raw_mode(void)
{
    tcgetattr(STDIN_FILENO, &orig_termios);

    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_cflag |= CS8;
    raw.c_oflag &= ~(OPOST);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

/* Restore original terminal settings */
static void disable_raw_mode(void)
{
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

/* Functions to manage block colors in the grid */
static void tui_set_block_color(int x, int y, int color)
{
    if (x >= 0 && x < GRID_WIDTH && y >= 0 && y < GRID_HEIGHT)
        color_grid[y][x] = color;
}

static int tui_get_block_color(int x, int y)
{
    if (x >= 0 && x < GRID_WIDTH && y >= 0 && y < GRID_HEIGHT)
        return color_grid[y][x];
    return 0;
}

/* Color scheme for different shapes */
static int get_falling_block_color(shape_t *shape)
{
    if (!shape)
        return 2;

    /* Assign colors based on shape type */
    uintptr_t addr = (uintptr_t) shape;
    int color_index = (addr % 7) + 2; /* Colors 2-8 */
    return color_index;
}

/* Function to build complete display state including falling block */
static void build_display_buffer(const grid_t *g, block_t *falling_block)
{
    /* Step 1: Clear display buffer completely */
    for (int row = 0; row < GRID_HEIGHT; row++) {
        for (int col = 0; col < GRID_WIDTH; col++) {
            display_buffer[row][col] = 0;
        }
    }

    /* Step 2: Add grid state (placed blocks) to buffer */
    for (int row = 0; row < g->height && row < GRID_HEIGHT; row++) {
        for (int col = 0; col < g->width && col < GRID_WIDTH; col++) {
            if (g->rows[row][col]) {
                int color = tui_get_block_color(col, row);
                display_buffer[row][col] = (color > 0) ? color : 7;
            }
        }
    }

    /* Step 3: Add falling block if it exists - COMPLETELY REWRITTEN */
    if (falling_block && falling_block->shape) {
        int falling_color = get_falling_block_color(falling_block->shape);

        /* Add all block parts that are within bounds */
        for (int i = 0; i < MAX_BLOCK_LEN; i++) {
            coord_t cr;
            block_get(falling_block, i, &cr);

            /* Only require that coordinate is within display buffer bounds */
            if (cr.x >= 0 && cr.x < GRID_WIDTH && cr.y >= 0 &&
                cr.y < GRID_HEIGHT) {
                /* Overwrite whatever is there - falling block takes priority
                 * for display */
                display_buffer[cr.y][cr.x] = falling_color;
            }
        }
    }

    display_buffer_valid = true;
}

/* Public function to build display buffer */
void tui_build_display_buffer(const grid_t *g, block_t *falling_block)
{
    build_display_buffer(g, falling_block);
}

/* Force display buffer refresh - invalidates current buffer */
void tui_force_display_buffer_refresh(void)
{
    display_buffer_valid = false;

    /* Also reset shadow buffer to force complete redraw */
    for (int y = 0; y < GRID_HEIGHT; y++) {
        for (int x = 0; x < GRID_WIDTH; x++) {
            shadow_board[y][x] = -1;
            display_buffer[y][x] = 0;
        }
    }
}

/* Render from display buffer */
void tui_render_display_buffer(const grid_t *g)
{
    if (!display_buffer_valid)
        return;

    for (int row = 0; row < g->height && row < GRID_HEIGHT; row++) {
        for (int col = 0; col < g->width && col < GRID_WIDTH; col++) {
            int color = display_buffer[row][col];

            /* In Tetris, row 0 is the bottom, but we display from top to
             * bottom, so we need to flip:
             * display_row = height - logical_row - 1
             */
            int display_y = g->height - row;
            /* This maps: row 0 -> display_y = 20, row 19 -> display_y = 1 */

            /* Always update if different to ensure proper display */
            if (shadow_board[row][col] != color) {
                draw_block(col, display_y, color);
                shadow_board[row][col] = color;
            }
        }
    }
    fflush(stdout);
}

static void show_sidebar_info(void)
{
    /* should be at position 32+ */
    int sidebar_x = 32;

    /* Level display */
    gotoxy(sidebar_x, 2);
    printf(COLOR_TEXT "Level  : %d" COLOR_RESET, 1);

    /* Points display */
    gotoxy(sidebar_x, 3);
    printf(COLOR_TEXT "Points : %d" COLOR_RESET, 0);

    /* Lines cleared display */
    gotoxy(sidebar_x, 4);
    printf(COLOR_TEXT "Lines  : %d" COLOR_RESET, 0);

    /* Mode display */
    gotoxy(sidebar_x, 5);
    printf(COLOR_TEXT "Mode   : Human" COLOR_RESET);

    /* Preview label */
    gotoxy(sidebar_x, 7);
    printf(COLOR_TEXT "Preview:" COLOR_RESET);

    /* Keys help */
    gotoxy(sidebar_x, 12);
    printf(COLOR_TEXT "Keys:" COLOR_RESET);

    gotoxy(sidebar_x, 13);
    printf(COLOR_TEXT "space   toggle AI/Human" COLOR_RESET);
    gotoxy(sidebar_x, 14);
    printf(COLOR_TEXT "p       pause" COLOR_RESET);
    gotoxy(sidebar_x, 15);
    printf(COLOR_TEXT "q       quit" COLOR_RESET);
    gotoxy(sidebar_x, 16);
    printf(COLOR_TEXT "arrows  move/rotate" COLOR_RESET);
}

void tui_grid_print(const grid_t *g)
{
    /* Grid rendering without falling block */
    for (int row = 0; row < g->height && row < GRID_HEIGHT; row++) {
        for (int col = 0; col < g->width && col < GRID_WIDTH; col++) {
            int color = 0;

            if (g->rows[row][col]) {
                color = tui_get_block_color(col, row);
                if (color == 0)
                    color = 7;
            }

            /* Fixed coordinate transformation to match display buffer */
            int display_y = g->height - row;

            if (shadow_board[row][col] != color) {
                draw_block(col, display_y, color);
                shadow_board[row][col] = color;
            }
        }
    }
    fflush(stdout);
}

void tui_block_print(block_t *b, int color, int grid_height)
{
    /* Direct block printing with corrected coordinate transformation */
    if (!b || !b->shape || color <= 0)
        return;

    for (int i = 0; i < MAX_BLOCK_LEN; i++) {
        coord_t cr;
        block_get(b, i, &cr);

        if (cr.x >= 0 && cr.x < GRID_WIDTH && cr.y >= 0 && cr.y < grid_height) {
            /* Fixed coordinate transformation */
            int display_y = grid_height - cr.y;
            draw_block(cr.x, display_y, color);

            /* Update shadow buffer to maintain consistency */
            if (cr.y >= 0 && cr.y < GRID_HEIGHT)
                shadow_board[cr.y][cr.x] = color;
        }
    }

    fflush(stdout);
}

void tui_block_print_preview(block_t *b, int color)
{
    /* Use same sidebar position as other functions */
    int sidebar_x = 32;

    /* Clear old preview area completely */
    for (int y = 8; y < 12; y++) {    /* 4 rows for preview */
        for (int x = 0; x < 8; x++) { /* 4 blocks * 2 chars each */
            gotoxy(sidebar_x + x, y);
            printf(" ");
            shadow_preview[y - 8][x / 2] = 0; /* Update shadow */
        }
    }

    /* Draw new preview */
    if (b && b->shape && color > 0) {
        for (int i = 0; i < MAX_BLOCK_LEN; i++) {
            coord_t cr;
            block_get(b, i, &cr);
            if (cr.x >= 0 && cr.x < 4 && cr.y >= 0 && cr.y < 4) {
                gotoxy(sidebar_x + cr.x * 2, 8 + cr.y);
                bgcolor(color, "  ");
                printf(COLOR_RESET);
                shadow_preview[cr.y][cr.x] = color; /* Update shadow */
            }
        }
    }
    fflush(stdout);
}

void tui_prompt(const grid_t *g, const char *msg)
{
    if (!msg)
        return;

    /* Position prompt in center of game board */
    gotoxy(g->width, g->height / 2 + 1);
    printf(COLOR_TEXT "%s" COLOR_RESET, msg);
    fflush(stdout);
}

void tui_block_print_shadow(block_t *b, int color, grid_t *g)
{
    /* shadow rendering with better validation */
    if (!b || !b->shape || !g || color <= 0)
        return;

    /* First add/remove the shadow */
    int r = b->offset.y;
    grid_block_drop(g, b);
    tui_block_print(b, color, g->height);
    b->offset.y = r;
    tui_block_print(b, color, g->height);
}

void tui_setup(const grid_t *g)
{
    if (tty_size() < 0)
        return;

    enable_raw_mode();

    /* Clear screen and hide cursor */
    printf(CLEAR_SCREEN HIDE_CURSOR);

    /* Initialize shadow buffers, color grid, and display buffer */
    for (int y = 0; y < GRID_HEIGHT; y++) {
        for (int x = 0; x < GRID_WIDTH; x++) {
            shadow_board[y][x] = -1;  /* Force redraw */
            color_grid[y][x] = 0;     /* No color initially */
            display_buffer[y][x] = 0; /* Empty display buffer */
        }
    }
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++)
            shadow_preview[y][x] = 0;
    }

    display_buffer_valid = false;

    /* Draw border */
    printf(COLOR_BORDER);

    /* Calculate border width: game width * 2 (for double-wide chars) */
    int border_width = g->width * 2;
    int right_border_pos = border_width + 1;

    /* Top border */
    gotoxy(0, 0);
    printf("+");
    for (int i = 0; i < border_width; i++)
        printf("-");
    printf("+");

    /* Side borders */
    for (int i = 1; i <= g->height; i++) {
        /* Left border */
        gotoxy(0, i);
        printf("|");
        /* Right border */
        gotoxy(right_border_pos, i);
        printf("|");
    }

    /* Bottom border */
    gotoxy(0, g->height + 1);
    printf("+");
    for (int i = 0; i < border_width; i++)
        printf("-");
    printf("+");

    printf(COLOR_RESET);

    /* Show sidebar information */
    show_sidebar_info();

    /* Clear the entire game area to ensure no artifacts */
    for (int y = 1; y <= g->height; y++) {
        for (int x = 1; x < right_border_pos; x++) {
            gotoxy(x, y);
            printf(" ");
        }
    }

    /* Draw the initial empty grid */
    tui_grid_print(g);

    fflush(stdout);
}

void tui_refresh(void)
{
    fflush(stdout);
}

void tui_quit(void)
{
    printf(SHOW_CURSOR CLEAR_SCREEN);
    gotoxy(1, 1);
    printf(COLOR_RESET);
    fflush(stdout);
    disable_raw_mode();
}

void tui_add_block_color(block_t *b, int color)
{
    if (!b || !b->shape)
        return;

    for (int i = 0; i < MAX_BLOCK_LEN; i++) {
        coord_t cr;
        block_get(b, i, &cr);
        /* Only store colors for valid grid positions */
        if (cr.x >= 0 && cr.x < GRID_WIDTH && cr.y >= 0 && cr.y < GRID_HEIGHT)
            tui_set_block_color(cr.x, cr.y, color);
    }
}

/* Global storage for color preservation during line clearing */
static int preserved_colors[GRID_WIDTH][GRID_HEIGHT];
static int preserved_counts[GRID_WIDTH];

void tui_prepare_color_preservation(const grid_t *g)
{
    /* Capture current color state before line clearing */
    for (int col = 0; col < GRID_WIDTH; col++) {
        preserved_counts[col] = 0;

        /* Collect colors in this column from bottom to top */
        for (int row = 0; row < g->height && row < GRID_HEIGHT; row++) {
            if (g->rows[row][col]) {
                int color = tui_get_block_color(col, row);
                if (color != 0)
                    preserved_colors[col][preserved_counts[col]++] = color;
            }
        }
    }
}

void tui_apply_color_preservation(const grid_t *g)
{
    /* Apply preserved colors to the new block positions after line clearing */

    /* Clear the color grid first */
    for (int y = 0; y < GRID_HEIGHT; y++) {
        for (int x = 0; x < GRID_WIDTH; x++) {
            color_grid[y][x] = 0;
        }
    }

    /* For each column, assign preserved colors to remaining blocks */
    for (int col = 0; col < g->width && col < GRID_WIDTH; col++) {
        int color_index = 0;

        /* Assign colors from bottom to top */
        for (int row = 0; row < g->height; row++) {
            if (g->rows[row][col] && color_index < preserved_counts[col]) {
                color_grid[row][col] = preserved_colors[col][color_index++];
            } else if (g->rows[row][col]) {
                /* No preserved color available, use default */
                color_grid[row][col] = 7;
            }
        }
    }

    /* Force redraw */
    tui_force_display_buffer_refresh();
}

void tui_clear_lines_colors(const grid_t *g)
{
    /* Legacy function - now handled by prepare/apply color preservation */
    /* Just force a redraw */
    tui_force_display_buffer_refresh();
}

/* Function to force complete grid redraw */
void tui_force_grid_redraw(void)
{
    /* Reset shadow buffer to force redraw of all grid cells */
    for (int y = 0; y < GRID_HEIGHT; y++) {
        for (int x = 0; x < GRID_WIDTH; x++)
            shadow_board[y][x] = -1;
    }
}

void tui_force_redraw(const grid_t *g)
{
    /* Clear the entire game area thoroughly */
    int right_border_pos = GRID_WIDTH * 2 + 1;
    printf(COLOR_RESET);

    for (int y = 1; y <= GRID_HEIGHT; y++) {
        gotoxy(1, y);
        for (int x = 1; x < right_border_pos; x++)
            printf(" ");
    }

    /* Force complete display buffer refresh */
    tui_force_display_buffer_refresh();

    /* Redraw everything from scratch using existing colors */
    tui_grid_print(g);

    /* Ensure borders are intact */
    tui_refresh_borders(g);

    /* Redraw sidebar */
    show_sidebar_info();

    fflush(stdout);
}

/* Reduced periodic cleanup to prevent flicker */
void tui_periodic_cleanup(const grid_t *g)
{
    /* Very light cleanup - only refresh borders occasionally */
    static int cleanup_counter = 0;
    cleanup_counter++;

    if (cleanup_counter % 300 == 0) { /* Even more reduced frequency */
        /* Only refresh borders to prevent border corruption */
        tui_refresh_borders(g);
        cleanup_counter = 0;
    }

    /* Every 100 frames, do a quick artifact check */
    if (cleanup_counter % 100 == 0) {
        /* Force display buffer refresh to ensure clean next render */
        tui_force_display_buffer_refresh();
    }
}

void tui_refresh_borders(const grid_t *g)
{
    /* Periodically refresh borders to prevent overwriting */
    printf(COLOR_BORDER);

    int border_width = g->width * 2;
    int right_border_pos = border_width + 1;

    /* Refresh left border */
    for (int i = 0; i <= g->height + 1; i++) {
        gotoxy(0, i);
        if (i == 0 || i == g->height + 1) {
            printf("+");
        } else {
            printf("|");
        }
    }

    /* Refresh right border */
    for (int i = 0; i <= g->height + 1; i++) {
        gotoxy(right_border_pos, i);
        if (i == 0 || i == g->height + 1) {
            printf("+");
        } else {
            printf("|");
        }
    }

    printf(COLOR_RESET);
}

void tui_update_stats(int level, int points, int lines_cleared)
{
    int sidebar_x = 32; /* Match show_sidebar_info */

    /* Update level */
    gotoxy(sidebar_x, 2);
    printf(COLOR_TEXT "Level  : %d" COLOR_RESET, level);

    /* Update points */
    gotoxy(sidebar_x, 3);
    printf(COLOR_TEXT "Points : %d" COLOR_RESET, points);

    /* Update lines cleared */
    gotoxy(sidebar_x, 4);
    printf(COLOR_TEXT "Lines  : %d" COLOR_RESET, lines_cleared);

    fflush(stdout);
}

void tui_update_mode_display(bool is_ai_mode)
{
    int sidebar_x = 32;

    /* Update mode display */
    gotoxy(sidebar_x, 5);
    printf(COLOR_TEXT "Mode   : %-6s" COLOR_RESET, is_ai_mode ? "AI" : "Human");

    fflush(stdout);
}

void tui_flash_completed_lines(const grid_t *g,
                               int *completed_rows,
                               int num_completed)
{
    if (num_completed <= 0)
        return;

    /* Flash effect for completed lines */
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

void tui_validate_line_clearing(const grid_t *g)
{
    int sidebar_x = 32;

    /* Check for completed lines */
    for (int row = 0; row < g->height; row++) {
        int filled_count = 0;
        for (int col = 0; col < g->width; col++) {
            if (g->rows[row][col])
                filled_count++;
        }

        if (filled_count == g->width) {
            /* Found a completed line that should be cleared */
            gotoxy(sidebar_x, 18);
            printf(COLOR_TEXT "Line %d ready to clear!" COLOR_RESET, row);
            fflush(stdout);
            break; /* Only show first completed line */
        }
    }
}

input_t tui_scankey(void)
{
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(STDIN_FILENO, &readfds);

    struct timeval timeout = {.tv_sec = 0, .tv_usec = 0};
    if (select(STDIN_FILENO + 1, &readfds, NULL, NULL, &timeout) > 0) {
        if (FD_ISSET(STDIN_FILENO, &readfds)) {
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
    }

    return INPUT_INVALID;
}
