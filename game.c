#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "nalloc.h"
#include "tetris.h"

/* FIXME: inaccurate */
#define SECOND 1e5

typedef enum { MOVE_LEFT, MOVE_RIGHT, DROP, ROTCW, ROTCCW, NONE } ui_move_t;

static ui_move_t move_next(grid_t *g, block_t *b, shape_stream_t *ss, float *w)
{
    static move_t *move = NULL;
    if (!move) {
        /* New block. just display it. */
        move = best_move(g, b, ss, w);
        return NONE;
    }

    /* Make moves one at a time. rotations first */
    if (b->rot != move->rot) {
        int inc = (move->rot - b->rot + 4) % 4;
        return inc < 3 ? ROTCW : ROTCCW;
    }

    if (b->offset.x != move->col)
        return move->col > b->offset.x ? MOVE_RIGHT : MOVE_LEFT;

    /* No further action. just drop */
    move = NULL;
    return DROP;
}

void auto_play(float *w)
{
    grid_t *g = grid_new(GRID_HEIGHT, GRID_WIDTH);
    block_t *b = block_new();

    tui_setup(g);

    bool dropped = true;
    shape_stream_t *ss = shape_stream_new();

    while (1) {
        switch (tui_scankey()) {
        case INPUT_PAUSE: {
            tui_prompt(g, "Paused");
            while (tui_scankey() != INPUT_PAUSE)
                usleep(0.1 * SECOND);
            tui_prompt(g, "      ");
            break;
        }
        case INPUT_QUIT:
            goto cleanup;
        case INPUT_INVALID:
            break;
        }

        if (dropped) {
            /* Generate a new block */
            shape_stream_pop(ss);
            block_init(b, shape_stream_peek(ss, 0));
            grid_block_center_elevate(g, b);
            /* If we can not place a new block, game over */
            if (grid_block_intersects(g, b))
                break;

            tui_block_print_shadow(b, 1 + 1, g);
            /* Simulate "wait! computer is thinking" */
            usleep(0.3 * SECOND);
            dropped = false;
        } else {
            ui_move_t move = move_next(g, b, ss, w);

            /* Simulate "wait! computer is thinking" */
            usleep(0.5 * SECOND);

            /* Unpaint old block */
            tui_block_print_shadow(b, 0, g);

            switch (move) {
            case MOVE_LEFT:
            case MOVE_RIGHT:
                grid_block_move(g, b, move == MOVE_LEFT ? LEFT : RIGHT, 1);
                break;

            case DROP:
                dropped = true;
                break;

            case ROTCW:
            case ROTCCW:
                grid_block_rotate(g, b, 1 + 2 * (move == ROTCCW));
                break;

            case NONE:
                break;
            }

            int cleared = 0;
            if (dropped) {
                grid_block_drop(g, b);
                grid_block_add(g, b);
                cleared = grid_clear_lines(g);
            }
            if (cleared) {
                /* Have to repaint the whole grid */
                tui_grid_print(g);
            } else {
                /* repaint in new location */
                tui_block_print_shadow(b, 1 + (int) (!dropped), g);
            }
        }
        tui_refresh();
    }

    tui_prompt(g, "Game Over!");
    sleep(3);
cleanup:
    tui_quit();
    nfree(ss);
    nfree(g);
    nfree(b);
}
