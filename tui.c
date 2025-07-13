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

/* Colors - matching tetris.c style */
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
static int color_grid[GRID_HEIGHT]
                     [GRID_WIDTH]; /* Store colors of placed blocks */

static void gotoxy(int x, int y)
{
    int xpos = (ttcols - MIN_COLS) / 2;
    int ypos = (ttrows - MIN_ROWS) / 2;

    printf(ESC "[%d;%dH", ypos + y, xpos + x);
}

static void draw_block(int x, int y, int color)
{
    /* Strict bounds checking to prevent any clutter */
    if (x < 0 || x >= GRID_WIDTH || y < 1 || y > GRID_HEIGHT)
        return; /* Out of bounds - don't draw */

    /* Calculate drawing position with border offset */
    int draw_x = x * 2 + 1;
    int right_border_pos = GRID_WIDTH * 2 + 1;

    /* Ensure we don't overwrite borders */
    if (draw_x >= 1 && draw_x < right_border_pos) {
        gotoxy(draw_x, y);
        if (color == 0) {
            /* Empty space - draw spaces to clear */
            printf("  ");
        } else {
            /* Colored block */
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

    /* Preview label */
    gotoxy(sidebar_x, 6);
    printf(COLOR_TEXT "Preview:" COLOR_RESET);

    /* Keys help - simplified for autoplay */
    gotoxy(sidebar_x, 11);
    printf(COLOR_TEXT "Keys:" COLOR_RESET);

    gotoxy(sidebar_x, 12);
    printf(COLOR_TEXT "space   pause" COLOR_RESET);
    gotoxy(sidebar_x, 13);
    printf(COLOR_TEXT "q       quit" COLOR_RESET);
}

void tui_grid_print(const grid_t *g)
{
    /* Print the entire grid systematically */
    for (int row = 0; row < g->height && row < GRID_HEIGHT; row++) {
        for (int col = 0; col < g->width && col < GRID_WIDTH; col++) {
            int color = 0;

            /* Determine color for this position */
            if (g->rows[row][col]) {
                /* There's a block here - get its stored color */
                color = tui_get_block_color(col, row);
                if (color == 0) {
                    /* No stored color - use a consistent default */
                    color = 7; /* White for any blocks without stored colors */
                }
            }
            /* If g->rows[row][col] is false, color stays 0 (empty) */

            /* Always draw to ensure clean display */
            int display_y = g->height - row;
            if (display_y >= 1 && display_y <= g->height) {
                draw_block(col, display_y, color);
                shadow_board[row][col] = color; /* Update shadow */
            }
        }
    }
    fflush(stdout);
}

void tui_block_print(block_t *b, int color, int grid_height)
{
    if (!b || !b->shape)
        return;

    for (int i = 0; i < MAX_BLOCK_LEN; i++) {
        coord_t cr;
        block_get(b, i, &cr);
        /* Only draw blocks within valid grid bounds */
        if (cr.x >= 0 && cr.x < GRID_WIDTH && cr.y >= 0 && cr.y < GRID_HEIGHT) {
            int display_y = grid_height - cr.y;
            draw_block(cr.x, display_y, color);
        }
    }
    fflush(stdout);
}

void tui_block_print_preview(block_t *b, int color)
{
    /* Use same sidebar position as other functions */
    int sidebar_x = 32;

    /* Clear old preview area completely */
    for (int y = 7; y < 11; y++) {    /* 4 rows for preview */
        for (int x = 0; x < 8; x++) { /* 4 blocks * 2 chars each */
            gotoxy(sidebar_x + x, y);
            printf(" ");
        }
    }

    /* Draw new preview */
    if (b && b->shape) {
        for (int i = 0; i < MAX_BLOCK_LEN; i++) {
            coord_t cr;
            block_get(b, i, &cr);
            if (cr.x >= 0 && cr.x < 4 && cr.y >= 0 && cr.y < 4) {
                gotoxy(sidebar_x + cr.x * 2, 7 + cr.y);
                bgcolor(color, "  ");
                printf(COLOR_RESET);
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

    /* Initialize shadow buffers and color grid */
    for (int y = 0; y < GRID_HEIGHT; y++) {
        for (int x = 0; x < GRID_WIDTH; x++) {
            shadow_board[y][x] = -1; /* Force redraw */
            color_grid[y][x] = 0;    /* No color initially */
        }
    }
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            shadow_preview[y][x] = 0;
        }
    }

    /* Draw border - visible like tetris.c */
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
        if (cr.x >= 0 && cr.x < GRID_WIDTH && cr.y >= 0 && cr.y < GRID_HEIGHT) {
            tui_set_block_color(cr.x, cr.y, color);
        }
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
    for (int y = 0; y < GRID_HEIGHT; y++) {
        for (int x = 0; x < GRID_WIDTH; x++)
            shadow_board[y][x] = -1;
    }
}

void tui_clear_lines_colors(const grid_t *g)
{
    /* Legacy function - now handled by prepare/apply color preservation */
    /* Just force a redraw */
    for (int y = 0; y < GRID_HEIGHT; y++) {
        for (int x = 0; x < GRID_WIDTH; x++) {
            shadow_board[y][x] = -1;
        }
    }
}

void tui_force_redraw(const grid_t *g)
{
    /* Complete screen cleanup - clear entire game area */
    int right_border_pos = GRID_WIDTH * 2 + 1;

    /* Clear the entire game area thoroughly */
    for (int y = 1; y <= GRID_HEIGHT; y++) {
        gotoxy(1, y);
        for (int x = 1; x < right_border_pos; x++) {
            printf("  ");
        }
    }

    /* Reset only shadow buffer - KEEP color information intact */
    for (int y = 0; y < GRID_HEIGHT; y++) {
        for (int x = 0; x < GRID_WIDTH; x++) {
            shadow_board[y][x] = -1;
        }
    }

    /* Redraw everything from scratch using existing colors */
    tui_grid_print(g);

    /* Ensure borders are intact */
    tui_refresh_borders(g);

    /* Redraw sidebar - this might be missing! */
    show_sidebar_info();
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
            if (g->rows[row][col]) {
                filled_count++;
            }
        }

        if (filled_count == g->width) {
            /* Found a completed line that should be cleared */
            gotoxy(sidebar_x, 16);
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
                return INPUT_PAUSE;
            case 'Q':
            case 'q':
                return INPUT_QUIT;
            default:
                return INPUT_INVALID;
            }
        }
    }

    return INPUT_INVALID;
}
