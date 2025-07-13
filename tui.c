#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

#include "tetris.h"

/* ANSI escape sequences */
#define ESC "\033"
#define CLEAR_SCREEN ESC "[2J" ESC "[H"
#define HIDE_CURSOR ESC "[?25l"
#define SHOW_CURSOR ESC "[?25h"
#define RESET_COLOR ESC "[0m"
#define MOVE_TO(row, col) printf(ESC "[%d;%dH", (row), (col))

/* Colors - using standard ANSI colors */
#define COLOR_RESET ESC "[0m"
#define COLOR_BORDER ESC "[32m" /* Green text */
#define COLOR_BLOCK ESC "[37m"  /* White text */
#define COLOR_ACTIVE ESC "[31m" /* Red text */
#define COLOR_BG ESC "[40m"     /* Black background */

static struct termios orig_termios;
static int start_row = 3;
static int start_col = 13;

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

static inline void tui_paint(int r, int c, int on)
{
    /* Game content is painted inside the border
     * Border occupies: start_row, start_row + g->height + 1, start_col,
     * start_col + g->width + 1
     * So game content goes from (start_row + 1, start_col + 1) to (start_row +
     * g->height, start_col + g->width)
     */

    MOVE_TO(start_row + 1 + r, start_col + 1 + c);

    if (on == 1) {
        printf(COLOR_BLOCK "#");
    } else if (on == 2) {
        printf(COLOR_ACTIVE "#");
    } else {
        printf(COLOR_RESET " ");
    }
    printf(COLOR_RESET);
}

void tui_grid_print(const grid_t *g)
{
    for (int row = g->height - 1; row >= 0; row--) {
        for (int col = 0; col < g->width; col++)
            tui_paint(g->height - 1 - row, col, g->rows[row][col]);
    }
}

void tui_block_print(block_t *b, int color, int grid_height)
{
    for (int i = 0; i < MAX_BLOCK_LEN; i++) {
        coord_t cr;
        block_get(b, i, &cr);
        tui_paint(grid_height - 1 - cr.y, cr.x, color);
    }
}

void tui_prompt(const grid_t *g, const char *msg)
{
    if (!msg)
        return;

    MOVE_TO(start_row + 1 + g->height / 2, start_col - 8);
    printf(COLOR_BLOCK "%s" COLOR_RESET, msg);
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
    enable_raw_mode();

    /* Clear screen and hide cursor */
    printf(CLEAR_SCREEN HIDE_CURSOR);
    fflush(stdout);

    /* Draw border using ASCII characters */
    printf(COLOR_BORDER);

    /* Top border */
    MOVE_TO(start_row, start_col);
    printf("+");
    for (int i = 0; i < g->width; i++)
        printf("-");
    printf("+");

    /* Side borders */
    for (int i = 0; i < g->height; i++) {
        /* Left border */
        MOVE_TO(start_row + 1 + i, start_col);
        printf("|");
        /* Right border */
        MOVE_TO(start_row + 1 + i, start_col + g->width + 1);
        printf("|");
    }

    /* Bottom border */
    MOVE_TO(start_row + g->height + 1, start_col);
    printf("+");
    for (int i = 0; i < g->width; i++)
        printf("-");
    printf("+");

    printf(COLOR_RESET);
    fflush(stdout);
}

void tui_refresh(void)
{
    fflush(stdout);
}

void tui_quit(void)
{
    printf(SHOW_CURSOR CLEAR_SCREEN);
    MOVE_TO(1, 1);
    printf(COLOR_RESET);
    fflush(stdout);
    disable_raw_mode();
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
