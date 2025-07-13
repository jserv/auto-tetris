#include <stdint.h>
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

/* Color scheme for different shapes - matches tetris.c */
static int get_shape_color(shape_t *shape)
{
    if (!shape)
        return 2;

    /* Assign colors based on shape type, similar to tetris.c */
    uintptr_t addr = (uintptr_t) shape;
    int color_index = (addr % 7) + 2; /* Colors 2-8 */
    return color_index;
}

void auto_play(float *w)
{
    grid_t *g = grid_new(GRID_HEIGHT, GRID_WIDTH);
    block_t *b = block_new();

    tui_setup(g);

    /* Game statistics */
    int total_points = 0;
    int total_lines_cleared = 0;
    int current_level = 1;

    /* Initialize and display sidebar with starting stats */
    tui_update_stats(current_level, total_points, total_lines_cleared);

    bool dropped = true;
    shape_stream_t *ss = shape_stream_new();
    int move_count = 0; /* Counter to periodically refresh borders */

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

            /* Show preview of next piece */
            block_t *preview_block = block_new();
            if (preview_block) {
                shape_t *next_shape = shape_stream_peek(ss, 1);
                if (next_shape) {
                    block_init(preview_block, next_shape);
                    int preview_color = get_shape_color(next_shape);
                    tui_block_print_preview(preview_block, preview_color);
                }
                nfree(preview_block);
            }

            int current_color = get_shape_color(b->shape);
            tui_block_print_shadow(b, current_color, g);
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
                /* Validate line clearing before dropping block */
                tui_validate_line_clearing(g);

                grid_block_drop(g, b);
                int current_color = get_shape_color(b->shape);
                grid_block_add(g, b);
                /* Store colors after adding to grid */
                tui_add_block_color(b, current_color);

                /* Check for completed lines and add visual feedback */
                int completed_rows[GRID_HEIGHT];
                int num_completed = 0;

                /* Find completed rows before clearing */
                for (int row = 0; row < g->height; row++) {
                    bool row_complete = true;
                    for (int col = 0; col < g->width; col++) {
                        if (!g->rows[row][col]) {
                            row_complete = false;
                            break;
                        }
                    }
                    if (row_complete)
                        completed_rows[num_completed++] = row;
                }

                /* Show visual feedback for completed lines */
                if (num_completed > 0) {
                    tui_flash_completed_lines(g, completed_rows, num_completed);

                    /* Capture colors BEFORE clearing lines */
                    tui_prepare_color_preservation(g);
                }

                cleared = grid_clear_lines(g);

                /* Apply color preservation AFTER clearing lines */
                if (cleared > 0)
                    tui_apply_color_preservation(g);

                /* Update statistics */
                if (cleared > 0) {
                    total_lines_cleared += cleared;
                    /* Points: 100 per line, bonus for multiple lines */
                    int line_points = cleared * 100;
                    if (cleared > 1)
                        line_points *= cleared; /* Bonus for multiple lines */
                    total_points += line_points;

                    /* Level up every 10 lines */
                    current_level = 1 + (total_lines_cleared / 10);

                    /* Update display */
                    tui_update_stats(current_level, total_points,
                                     total_lines_cleared);
                }
            }
            if (cleared) {
                /* Force clean redraw after line clearing with preserved colors
                 */
                tui_force_redraw(g);
                /* Make sure stats are updated after redraw */
                tui_update_stats(current_level, total_points,
                                 total_lines_cleared);
            } else {
                /* repaint in new location */
                int current_color = get_shape_color(b->shape);
                tui_block_print_shadow(b, current_color, g);
            }

            /* Periodically refresh borders to prevent clutter accumulation */
            move_count++;
            if (move_count % 10 == 0) {
                tui_refresh_borders(g);
                /* Also refresh sidebar to ensure it stays visible */
                tui_update_stats(current_level, total_points,
                                 total_lines_cleared);
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
    free_shape();
}
