#include <math.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "nalloc.h"
#include "tetris.h"

/* FIXME: inaccurate */
#define SECOND 1e5

/* NES-authentic gravity speeds (frames per drop at 60 FPS) */
static const int NES_GRAVITY_SPEEDS[29] = {
    48, 43, 38, 33, 28, 23, 18, 13, 8, 6, 5, 5, 5, 4, 4,
    4,  3,  3,  3,  2,  2,  2,  2,  2, 2, 2, 2, 2, 2,
};

/* NES-authentic line clear scoring rewards */
static const unsigned short NES_CLEAR_REWARDS[5] = {0, 40, 100, 300, 1200};

/* NES timing constants */
#define NES_FRAME_US 16667    /* 16.667ms per frame at 60 FPS */
#define ENTRY_DELAY_FRAMES 10 /* Brief pause for new piece */

typedef enum { MOVE_LEFT, MOVE_RIGHT, DROP, ROTCW, ROTCCW, NONE } ui_move_t;

/* Game state variables */
static bool is_ai_mode = false; /* Start in human mode */
static bool game_running = true;

/* NES-specific timing state */
static int gravity_counter = 0;
static int entry_delay_counter = 0;

static ui_move_t move_next(grid_t *g, block_t *b, shape_stream_t *ss, float *w)
{
    static move_t *move = NULL;
    static shape_t *last_shape = NULL;
    static coord_t last_offset = {-1, -1};

    /* Check if we need to reset due to block/position change (mode switch or
     * new block).
     */
    if (!move || last_shape != b->shape || last_offset.x != b->offset.x ||
        last_offset.y != b->offset.y) {
        /* Block changed or position significantly different - recalculate */
        move = move_find_best(g, b, ss, w);
        /* move_find_best() can return NULL (e.g. OOM). Fallback to hard‑drop
         * instead of dereferencing a NULL pointer next frame.
         */
        if (!move)
            return DROP;

        last_shape = b->shape;
        last_offset = b->offset;
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
    last_shape = NULL;
    last_offset.x = -1;
    last_offset.y = -1;
    return DROP;
}

/* Get NES-authentic gravity delay for given level */
static int get_gravity_delay(int level)
{
    /* Convert 1-based level to 0-based array index */
    int level_index = (level - 1);
    if (level_index < 0)
        level_index = 0;
    if (level_index >= 29)
        level_index = 28;

    return NES_GRAVITY_SPEEDS[level_index];
}

/* Calculate NES-authentic score for line clears */
static int calculate_nes_score(int lines_cleared, int total_lines_cleared)
{
    if (lines_cleared < 0 || lines_cleared > 4)
        return 0;

    /* NES level calculation: level = total_lines_cleared / 10 */
    int level = total_lines_cleared / 10;

    /* NES scoring formula: base_points * (level + 1) */
    return NES_CLEAR_REWARDS[lines_cleared] * (level + 1);
}

/* Frame-based timing for gravity */
static bool should_drop_piece(int level)
{
    int required_frames = get_gravity_delay(level);

    if (++gravity_counter >= required_frames) {
        gravity_counter = 0;
        return true;
    }

    return false;
}

/* Handle entry delay for new pieces */
static bool is_entry_delay_active(void)
{
    if (entry_delay_counter > 0) {
        entry_delay_counter--;
        return true;
    }
    return false;
}

/* Start entry delay for new piece */
static void start_entry_delay(void)
{
    entry_delay_counter = ENTRY_DELAY_FRAMES;
    gravity_counter = 0; /* Reset gravity timing for new piece */
}

/* CPU-friendly pause input handling with blocking poll */
static input_t pause_scankey(void)
{
    struct pollfd pfd = {.fd = STDIN_FILENO, .events = POLLIN, .revents = 0};

    /* Block indefinitely until input arrives - zero CPU usage */
    if ((poll(&pfd, 1, -1) > 0) && (pfd.revents & POLLIN)) {
        char c = getchar();
        switch (c) {
        case 'p':
        case 'P':
            return INPUT_PAUSE;
        case 'Q':
        case 'q':
            return INPUT_QUIT;
        default:
            return INPUT_INVALID;
        }
    }

    return INPUT_INVALID;
}

/* Benchmark mode: Run a single game without TUI and return statistics */
game_stats_t bench_run_single(float *w,
                              int *total_pieces_so_far,
                              int total_expected_pieces)
{
    game_stats_t stats = {0, 0, 0, 0.0f, 0.0, false, 0.0f};
    if (!w)
        return stats;

    /* Start timing */
    struct timespec start_time, end_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    grid_t *g = grid_new(GRID_HEIGHT, GRID_WIDTH);
    block_t *b = block_new();
    shape_stream_t *ss = shape_stream_new();

    if (!g || !b || !ss) {
        /* Cleanup allocated resources before returning */
        nfree(g);
        nfree(b);
        nfree(ss);
        return stats;
    }

    /* Game statistics */
    int total_points = 0;
    int total_lines_cleared = 0;
    int pieces_placed = 0;

    /* Configurable safety limits */
    const int MAX_PIECES = 5000;      /* Reasonable limit for benchmarking */
    const int MAX_MOVE_ATTEMPTS = 20; /* Maximum attempts per move */
    /* Update progress every N pieces */
    const int PROGRESS_UPDATE_INTERVAL = 25;
    const int progress_width = 40;

    /* Initialize first block */
    shape_stream_pop(ss);
    shape_t *first_shape = shape_stream_peek(ss, 0);
    if (!first_shape)
        goto cleanup;

    block_init(b, first_shape);
    grid_block_spawn(g, b);

    /* Check initial placement */
    if (grid_block_collides(g, b))
        goto cleanup;

    pieces_placed = 1;

    /* Main game loop - AI only mode */
    while (pieces_placed < MAX_PIECES) {
        /* Direct AI decision: get best move */
        move_t *best = move_find_best(g, b, ss, w);

        if (!best) /* No valid move found, natural game over */
            break;

        /* Validate the AI's suggested move */
        block_t test_block = *b;
        test_block.rot = best->rot;
        test_block.offset.x = best->col;

        /* Check if the suggested position is valid */
        if (grid_block_collides(g, &test_block))
            break;

        /* Apply rotation with safety limit */
        int rotation_attempts = 0;
        while (b->rot != best->rot && rotation_attempts < MAX_MOVE_ATTEMPTS) {
            int old_rot = b->rot;
            grid_block_rotate(g, b, 1);

            /* Check if rotation made progress */
            if (b->rot == old_rot)
                break;

            rotation_attempts++;
        }

        /* Apply horizontal movement with safety limit */
        int movement_attempts = 0;
        while (b->offset.x != best->col &&
               movement_attempts < MAX_MOVE_ATTEMPTS) {
            int old_x = b->offset.x;

            if (b->offset.x < best->col) {
                grid_block_move(g, b, RIGHT, 1);
            } else {
                grid_block_move(g, b, LEFT, 1);
            }

            /* Check if movement made progress */
            if (b->offset.x == old_x)
                break;

            movement_attempts++;
        }

        /* Drop the piece */
        grid_block_drop(g, b);

        /* Verify piece is in valid position before adding */
        if (grid_block_collides(g, b))
            break;

        /* Add block to grid */
        grid_block_add(g, b);

        /* Clear completed lines */
        int cleared = grid_clear_lines(g);
        if (cleared > 0) {
            total_lines_cleared += cleared;

            /* Calculate points using authentic NES scoring */
            int nes_points = calculate_nes_score(cleared, total_lines_cleared);
            total_points += nes_points;
        }

        /* Generate next block */
        shape_stream_pop(ss);
        shape_t *next_shape = shape_stream_peek(ss, 0);
        if (!next_shape)
            break;

        block_init(b, next_shape);
        grid_block_spawn(g, b);

        /* Check for game over */
        if (grid_block_collides(g, b))
            break;

        pieces_placed++;

        /* Update progress bar periodically */
        if (pieces_placed % PROGRESS_UPDATE_INTERVAL == 0) {
            /* Calculate current total progress across all games */
            int current_total =
                (total_pieces_so_far ? *total_pieces_so_far : 0) +
                pieces_placed;

            /* Bounds check for progress calculation */
            if (total_expected_pieces > 0) {
                /* Simple progress bar update - overwrite the existing line */
                printf("\r\x1b[K"); /* Clear current line */
                printf("Progress: [");

                /* Calculate filled characters */
                int filled =
                    (current_total * progress_width) / total_expected_pieces;

                /* Bounds check for progress bar */
                if (filled > progress_width)
                    filled = progress_width;

                /* Draw progress bar with simple blocks */
                printf("\x1b[32m"); /* Green color */
                for (int i = 0; i < filled; i++)
                    printf("█");
                printf("\x1b[0m"); /* Reset color */
                for (int i = filled; i < progress_width; i++)
                    printf(" ");

                printf("] %d/%d pieces", current_total, total_expected_pieces);
                fflush(stdout);
            }
        }
    }

    /* Record if we hit the artificial limit */
    if (pieces_placed >= MAX_PIECES)
        stats.hit_piece_limit = true;

cleanup:
    /* End timing */
    clock_gettime(CLOCK_MONOTONIC, &end_time);
    double duration = (end_time.tv_sec - start_time.tv_sec) +
                      (end_time.tv_nsec - start_time.tv_nsec) / 1000000000.0;

    /* Calculate final statistics */
    stats.lines_cleared = total_lines_cleared;
    stats.score = total_points;
    stats.pieces_placed = pieces_placed;
    stats.lcpp =
        pieces_placed > 0 ? (float) total_lines_cleared / pieces_placed : 0.0f;
    stats.game_duration = duration;
    stats.pieces_per_second = duration > 0 ? pieces_placed / duration : 0.0f;

    /* Ensure cleanup always happens */
    nfree(ss);
    nfree(g);
    nfree(b);

    return stats;
}

/* Run benchmark with multiple games */
bench_results_t bench_run_multi(float *weights, int num_games)
{
    bench_results_t results = {0};
    if (!weights || num_games <= 0)
        return results;

    /* Bounds check for reasonable number of games */
    if (num_games > 10000) {
        printf("Warning: Limiting games to 10000 for memory safety\n");
        num_games = 10000;
    }

    results.games = malloc(num_games * sizeof(game_stats_t));
    if (!results.games) {
        printf("Error: Failed to allocate memory for %d games\n", num_games);
        return results;
    }

    /* Initialize games array to prevent undefined behavior */
    memset(results.games, 0, num_games * sizeof(game_stats_t));

    results.num_games = num_games;
    results.natural_endings = 0;

    /* Initialize totals for averaging */
    int total_lines = 0;
    int total_score = 0;
    int total_pieces = 0;
    float total_lcpp = 0.0f;
    double total_duration = 0.0;
    float total_pps = 0.0f;

    printf("Running %d benchmark games...\n", num_games);

    /* Always show initial progress bar */
    const int progress_width = 40;
    const int max_pieces_per_game =
        5000; /* Should match MAX_PIECES in bench_run_single */
    const int total_expected_pieces = num_games * max_pieces_per_game;

    printf("Progress: [");
    for (int i = 0; i < progress_width; i++)
        printf(" ");
    printf("] 0/%d pieces", total_expected_pieces);

    /* Run games with progress reporting */
    for (int i = 0; i < num_games; i++) {
        results.games[i] =
            bench_run_single(weights, &total_pieces, total_expected_pieces);

        /* Track natural vs artificial endings */
        if (!results.games[i].hit_piece_limit)
            results.natural_endings++;

        /* Validate results (sanity check) */
        if (results.games[i].pieces_placed <= 0) {
            printf("Warning: Game %d produced invalid results\n", i + 1);
            /* Set minimal valid stats to avoid division by zero */
            results.games[i].pieces_placed = 1;
            results.games[i].lcpp = 0.0f;
        }

        /* Update totals */
        total_lines += results.games[i].lines_cleared;
        total_score += results.games[i].score;
        total_pieces += results.games[i].pieces_placed;
        total_lcpp += results.games[i].lcpp;
        total_duration += results.games[i].game_duration;
        total_pps += results.games[i].pieces_per_second;

        /* Track best performance */
        if (i == 0 ||
            results.games[i].lines_cleared > results.best.lines_cleared) {
            results.best = results.games[i];
        }

        /* Progress bar update */
        if (total_expected_pieces > 0) {
            printf("\r\x1b[K"); /* Clear current line */
            printf("Progress: [");

            /* Calculate filled characters */
            int filled =
                (total_pieces * progress_width) / total_expected_pieces;
            if (filled > progress_width)
                filled = progress_width;

            /* Draw progress bar with simple blocks */
            printf("\x1b[32m"); /* Green color */
            for (int j = 0; j < filled; j++)
                printf("█");
            printf("\x1b[0m"); /* Reset color */
            for (int j = filled; j < progress_width; j++)
                printf(" ");

            printf("] %d/%d pieces", total_pieces, total_expected_pieces);
            fflush(stdout);
        }

        results.total_games_completed++;
    }

    /* Clear progress bar area when done */
    printf("\nCompleted %d pieces across %d games.\n", total_pieces, num_games);

    /* Calculate averages */
    if (results.total_games_completed > 0) {
        results.avg.lines_cleared = total_lines / results.total_games_completed;
        results.avg.score = total_score / results.total_games_completed;
        results.avg.pieces_placed =
            total_pieces / results.total_games_completed;
        results.avg.lcpp = total_lcpp / results.total_games_completed;
        results.avg.game_duration =
            total_duration / results.total_games_completed;
        results.avg.pieces_per_second =
            total_pps / results.total_games_completed;
        results.avg.hit_piece_limit =
            false; /* Average doesn't make sense for boolean */
    }

    return results;
}

/* Print benchmark results */
void bench_print(const bench_results_t *results)
{
    if (!results || results->total_games_completed == 0) {
        printf("No benchmark results to display.\n");
        return;
    }

    printf("\n=== Results ===\n");
    printf("Games completed: %d/%d\n", results->total_games_completed,
           results->num_games);

    printf("\nAverage Performance:\n");
    printf("  Lines Cleared:     %d\n", results->avg.lines_cleared);
    printf("  Score:             %d\n", results->avg.score);

    /* Calculate total pieces across all games */
    int total_pieces = 0;
    for (int i = 0; i < results->total_games_completed; i++)
        total_pieces += results->games[i].pieces_placed;
    printf("  Pieces Placed:     %d\n", total_pieces);

    printf("  LCPP:              %.3f\n", results->avg.lcpp);
    printf("  Game Duration:     %.1f seconds\n", results->avg.game_duration);

    printf("  Search Speed:      %.1f pieces/second\n",
           results->avg.pieces_per_second);

    printf("========================\n");
}

void game_run(float *w)
{
    /* Validate input parameter */
    if (!w) {
        printf("Error: Invalid weights provided\n");
        return;
    }

    grid_t *g = grid_new(GRID_HEIGHT, GRID_WIDTH);
    block_t *b = block_new();
    shape_stream_t *ss = shape_stream_new();

    if (!g || !b || !ss) {
        printf("Error: Failed to allocate game resources\n");
        /* Cleanup any successful allocations */
        if (g)
            nfree(g);
        if (b)
            nfree(b);
        if (ss)
            nfree(ss);
        return;
    }

    tui_setup(g);

    /* Game statistics */
    int total_points = 0;
    int total_lines_cleared = 0;
    int current_level = 1;

    /* Game mode state */
    bool is_paused = false;
    bool dropped = false; /* First block will be initialized before loop */
    int move_count = 0;

    /* Initialize and display sidebar with starting stats */
    tui_update_stats(current_level, total_points, total_lines_cleared);
    tui_update_mode_display(is_ai_mode);

    /* Initialize first block before main loop */
    shape_stream_pop(ss);
    shape_t *first_shape = shape_stream_peek(ss, 0);
    if (!first_shape) {
        tui_prompt(g, "Error: No shapes available!");
        sleep(3);
        goto cleanup;
    }

    block_init(b, first_shape);
    grid_block_spawn(g, b);

    /* Validate first block placement */
    if (grid_block_collides(g, b)) {
        tui_animate_gameover(g);
        tui_prompt(g, "Game Over!");
        sleep(3);
        goto cleanup;
    }

    /* Always show preview of next piece, ensuring it is visible from start */
    block_t *preview_block = block_new();
    if (preview_block) {
        shape_t *next_shape = shape_stream_peek(ss, 1);
        if (next_shape) {
            block_init(preview_block, next_shape);
            int preview_color = tui_get_shape_color(next_shape);
            tui_show_preview(preview_block, preview_color);
        }
        nfree(preview_block);
    }

    /* Start entry delay for first piece */
    start_entry_delay();
    gravity_counter = 0; /* Initialize gravity timing */

    /* Force immediate display buffer build and render for first block */
    tui_build_buffer(g, b);
    tui_render_buffer(g);
    tui_refresh();

    while (game_running) {
        /* CPU-friendly pause handling with blocking poll */
        if (is_paused) {
            /* Block until input; zero CPU while the game is paused */
            while (is_paused) {
                input_t pause_input = pause_scankey();
                if (pause_input == INPUT_QUIT)
                    goto cleanup;

                if (pause_input == INPUT_PAUSE) {
                    is_paused = false;
                    /* Force complete redraw to clear pause message */
                    tui_force_redraw(g);
                    /* Resume with frame-based timing */
                }
            }

            /* Resume normal rendering after unpause */
            tui_build_buffer(g, b);
            tui_render_buffer(g);
            tui_refresh();
            continue;
        }

        if (dropped) {
            /* Generate next block */
            shape_stream_pop(ss);
            shape_t *next_shape = shape_stream_peek(ss, 0);
            /* No more shapes available - shouldn't happen */
            if (!next_shape)
                break;

            block_init(b, next_shape);
            grid_block_spawn(g, b);

            /* Start entry delay for new piece */
            start_entry_delay();

            /* Check for game over */
            if (grid_block_collides(g, b))
                break;

            /* Always update preview after generating new block */
            block_t *preview_block = block_new();
            if (preview_block) {
                shape_t *preview_shape = shape_stream_peek(ss, 1);
                if (preview_shape) {
                    block_init(preview_block, preview_shape);
                    int preview_color = tui_get_shape_color(preview_shape);
                    tui_show_preview(preview_block, preview_color);
                } else {
                    /* Clear preview if no next shape available */
                    tui_show_preview(NULL, 0);
                }
                nfree(preview_block);
            } else {
                /* Clear preview if preview block creation failed */
                tui_show_preview(NULL, 0);
            }

            dropped = false;

            /* Force display buffer refresh when new block appears */
            tui_refresh_force();
        }

        /* Always rebuild and render each frame */

        /* Step 1: Always build complete display state (grid + falling block) */
        tui_build_buffer(g, b);

        /* Step 2: Always render from display buffer */
        tui_render_buffer(g);

        /* Step 3: Get input after rendering is complete */
        input_t input = tui_scankey();

        /* Handle global commands */
        switch (input) {
        case INPUT_TOGGLE_MODE: {
            is_ai_mode = !is_ai_mode;
            /* Force one clean redraw after mode switch */
            tui_force_redraw(g);
            tui_update_mode_display(is_ai_mode);
            /* Rebuild display after mode switch */
            tui_build_buffer(g, b);
            tui_render_buffer(g);
            continue; /* Skip movement this frame */
        }
        case INPUT_PAUSE: {
            is_paused = true;
            tui_prompt(g, "Paused - Press 'p' to resume");
            continue; /* Skip movement this frame */
        }
        case INPUT_QUIT:
            goto cleanup;
        default:
            break;
        }

        /* Handle NES-authentic gravity and entry delay */
        if (!is_ai_mode && !is_entry_delay_active()) {
            if (should_drop_piece(current_level)) {
                /* Check if piece can move down before attempting move */
                int old_y = b->offset.y;
                grid_block_move(g, b, BOT, 1);

                /* If position didn't change, piece hit bottom/obstacle */
                if (b->offset.y == old_y)
                    dropped = true;
            }
        }

        /* Handle movement input with consistent collision detection */
        if (!dropped && !is_entry_delay_active()) {
            if (is_ai_mode) {
                /* AI mode with thinking simulation */
                ui_move_t ai_move = move_next(g, b, ss, w);

                /* Add timeout protection to prevent AI stalling */
                static int ai_timeout_counter = 0;
                static int ai_thinking_delay = 0;

                if (ai_move == NONE) {
                    ai_timeout_counter++;
                    ai_thinking_delay++;

                    /* Simulate AI thinking with much slower, more human-like
                     * delay
                     */
                    if (ai_thinking_delay < 2) {
                        usleep(0.8 * SECOND); /* Deep initial thinking */
                    } else if (ai_thinking_delay < 4) {
                        usleep(0.5 * SECOND); /* Continued analysis */
                    } else if (ai_thinking_delay < 6) {
                        usleep(0.3 * SECOND); /* Final consideration */
                    } else {
                        usleep(0.2 * SECOND); /* Quick final check */
                    }

                    if (ai_timeout_counter > 20) {
                        /* AI seems stuck, force a drop to continue game */
                        ai_move = DROP;
                        ai_timeout_counter = 0;
                        ai_thinking_delay = 0;
                    }
                } else {
                    ai_timeout_counter =
                        0; /* Reset counter on successful move */
                    ai_thinking_delay = 0;

                    /* Simulate human-like execution delays for moves */
                    switch (ai_move) {
                    case ROTCW:
                    case ROTCCW:
                        usleep(0.4 * SECOND); /* Rotation needs deliberation */
                        break;
                    case MOVE_LEFT:
                    case MOVE_RIGHT:
                        usleep(0.25 * SECOND); /* Movement takes time */
                        break;
                    case DROP:
                        usleep(0.6 *
                               SECOND); /* Drop needs confirmation pause */
                        break;
                    default:
                        break;
                    }
                }

                switch (ai_move) {
                case MOVE_LEFT:
                    grid_block_move(g, b, LEFT, 1);
                    break;
                case MOVE_RIGHT:
                    grid_block_move(g, b, RIGHT, 1);
                    break;
                case DROP:
                    grid_block_drop(g, b);
                    dropped = true;
                    break;
                case ROTCW:
                    grid_block_rotate(g, b, 1);
                    break;
                case ROTCCW:
                    grid_block_rotate(g, b, 3);
                    break;
                case NONE:
                    /* AI is still thinking, continue loop */
                    break;
                }
            } else {
                /* Human mode now uses same validation functions */
                switch (input) {
                case INPUT_MOVE_LEFT:
                    grid_block_move(g, b, LEFT, 1);
                    break;
                case INPUT_MOVE_RIGHT:
                    grid_block_move(g, b, RIGHT, 1);
                    break;
                case INPUT_ROTATE:
                    grid_block_rotate(g, b, 1);
                    break;
                case INPUT_DROP:
                    /* Hard drop - instant drop to bottom */
                    grid_block_drop(g, b);
                    dropped = true;
                    break;
                default:
                    break;
                }
            }
        }

        /* Handle piece placement and line clearing if dropped */
        if (dropped) {
            /* Add piece to grid */
            if (!grid_block_collides(g, b)) {
                /* Get color BEFORE adding block to grid */
                int current_color = tui_get_shape_color(b->shape);

                /* Add block to grid */
                grid_block_add(g, b);

                /* Preserve the color assignment */
                tui_add_block_color(b, current_color);

                /* Prepare color preservation before checking for lines */
                tui_save_colors(g);

                /* Check for completed lines */
                int completed_rows[GRID_HEIGHT];
                int num_completed = 0;

                /* Find completed rows */
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

                /* Show animation if lines completed */
                if (num_completed > 0) {
                    /* Build and render clean state first */

                    /* No falling block during animation */
                    tui_build_buffer(g, NULL);
                    tui_render_buffer(g);
                    tui_refresh();

                    /* Now show line clearing animation */
                    tui_flash_lines(g, completed_rows, num_completed);
                }

                /* Clear lines using grid function */
                int cleared = grid_clear_lines(g);

                if (cleared > 0) {
                    /* Apply color preservation after line clearing */
                    tui_restore_colors(g);

                    /* Force complete cleanup after line clearing */
                    tui_force_redraw(g);

                    /* Update statistics using NES scoring */
                    total_lines_cleared += cleared;
                    int nes_points =
                        calculate_nes_score(cleared, total_lines_cleared);
                    total_points += nes_points;

                    /* Calculate new level (every 10 lines) */
                    int new_level = 1 + (total_lines_cleared / 10);
                    if (new_level != current_level) {
                        current_level = new_level;
                        /* Level change is now handled by frame-based timing */
                    }

                    /* Update UI after statistics change */
                    tui_update_stats(current_level, total_points,
                                     total_lines_cleared);
                    tui_update_mode_display(is_ai_mode);
                } else {
                    /* No lines cleared, just force display buffer refresh */
                    tui_refresh_force();
                }
            }
        }

        /* Periodic maintenance and ensure preview stays visible */
        move_count++;
        /* Reduced frequency to prevent flickering */
        if (move_count % 200 == 0) {
            tui_refresh_borders(g);
            tui_update_stats(current_level, total_points, total_lines_cleared);
            tui_update_mode_display(is_ai_mode);

            /* Refresh preview to ensure it stays visible */
            block_t *refresh_preview = block_new();
            if (refresh_preview) {
                shape_t *next_shape = shape_stream_peek(ss, 1);
                if (next_shape) {
                    block_init(refresh_preview, next_shape);
                    int preview_color = tui_get_shape_color(next_shape);
                    tui_show_preview(refresh_preview, preview_color);
                }
                nfree(refresh_preview);
            }
        }

        /* Use very reduced periodic cleanup to prevent artifacts without
         * flickering
         */
        if (move_count % 1000 == 0) /* only every 1000 frames */
            tui_cleanup_display(g);

        /* Always refresh after each frame to ensure display is current */
        tui_refresh();

        /* Frame-rate control: 60 FPS for NES authenticity and gravity timing */
        usleep(NES_FRAME_US);
    }

    tui_animate_gameover(g);
    tui_prompt(g, "Game Over!");
    sleep(3);
cleanup:
    tui_quit();
    /* Ensure cleanup always happens */
    nfree(ss);
    nfree(g);
    nfree(b);
    shape_free();
}
