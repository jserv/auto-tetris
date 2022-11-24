#include <ncurses.h>

#include "tetris.h"

#define EDGE 1

static WINDOW *win;

static inline void tui_paint(int r, int c, int on)
{
    r += EDGE;
    c += EDGE;

    int pair = 1 + on;
    wattron(win, COLOR_PAIR(pair));
    mvwaddch(win, r, c, ' ');
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

    mvprintw(g->height / 2, 2, msg);
    refresh();
}

void tui_block_print_shadow(block_t *b, int color, grid_t *g)
{
    // first add/remove the shadow
    int r = b->offset.y;
    grid_block_drop(g, b);
    tui_block_print(b, color, g->height);
    b->offset.y = r;
    tui_block_print(b, color, g->height);
}

void tui_setup(const grid_t *g)
{
    int startrc[] = {2, 12};

    /* Initialize the standard screen */
    initscr();

    /* Allow EDGE units on each side for border */
    win = newwin(g->height + EDGE * 2, g->width + EDGE * 2, startrc[0],
                 startrc[1]);

    /* Don't echo keypresses like ^[[A */
    noecho();

    /* Don't echo a blinking cursor */
    curs_set(0);

    /* Needed before any color-related calls */
    start_color();
    int BG = COLOR_BLACK;
    int FG = COLOR_WHITE;
    int UNUSED = COLOR_BLUE;
    init_pair(1, UNUSED, BG);        /* blank */
    init_pair(2, UNUSED, FG);        /* dropped blocks */
    init_pair(3, UNUSED, COLOR_RED); /* active block */
    init_pair(4, FG, COLOR_GREEN);   /* border */
    assume_default_colors(FG, BG);

    wattron(win, COLOR_PAIR(4)); /* border color */
    box(win, ACS_VLINE, ACS_HLINE);

    keypad(win, TRUE);
    nodelay(win, TRUE);

    tui_refresh();
}

void tui_refresh(void)
{
    wrefresh(stdscr);
    wrefresh(win);
}

void tui_quit(void)
{
    endwin();
}

input_t tui_scankey(void)
{
    switch (wgetch(win)) {
    case ' ':
        return INPUT_PAUSE;
    case 'Q':
    case 'q':
        return INPUT_QUIT;
    default:
        return INPUT_INVALID;
    }
}
