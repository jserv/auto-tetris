#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "nalloc.h"
#include "tetris.h"

/* FIXME: inaccurate */
#define SECOND 1e5

typedef enum { MOVE_LEFT, MOVE_RIGHT, DROP, ROTCW, ROTCCW, NONE } ui_move_t;

/* Game state variables */
static volatile sig_atomic_t auto_drop_ready = 0;
static volatile sig_atomic_t is_ai_mode = 0; /* Start in human mode */
static volatile sig_atomic_t game_running = 1;

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
        move = best_move(g, b, ss, w);
        /* best_move() can return NULL (e.g. OOM). Fallback to hard‑drop instead
         * of dereferencing a NULL pointer next frame.
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

/* Timer handler for automatic piece dropping in human mode */
static void alarm_handler(int signo)
{
    static long h[4];

    (void) signo;

    /* On init, set initial interval */
    if (!signo) {
        h[3] = 800000; /* 0.8 seconds initial - slower like original */
        return;
    }

    if (!is_ai_mode) {
        auto_drop_ready = 1;
        /* Gradually decrease interval as in original tetris, but keep
         * reasonable for human play */
        h[3] -= h[3] / 3000; /* Gradual speedup */
        if (h[3] < 100000)
            h[3] = 100000; /* Don't go below 0.1 seconds */
        setitimer(ITIMER_REAL, (struct itimerval *) h, 0);
    }
}

/* Initialize timer system */
static void init_timer(void)
{
    struct sigaction sa;

    sigemptyset(&sa.sa_mask);
    sigaddset(&sa.sa_mask, SIGALRM);
    sa.sa_flags = 0;
    sa.sa_handler = alarm_handler;
    sigaction(SIGALRM, &sa, NULL);

    /* Start timer */
    alarm_handler(0);
    alarm_handler(SIGALRM);
}

/* Stop timer */
static void stop_timer(void)
{
    struct itimerval timer = {0};
    setitimer(ITIMER_REAL, &timer, 0);
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
game_stats_t bench_play_single(float *w,
                               int *total_pieces_so_far,
                               int total_expected_pieces)
{
    game_stats_t stats = {0, 0, 0, 0.0f, 0.0, false, 0.0f};

    /* Start timing */
    struct timespec start_time, end_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    grid_t *g = grid_new(GRID_HEIGHT, GRID_WIDTH);
    block_t *b = block_new();
    shape_stream_t *ss = shape_stream_new();

    if (!g || !b || !ss) {
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
    block_init(b, shape_stream_peek(ss, 0));
    grid_block_center_elevate(g, b);

    /* Check initial placement */
    if (grid_block_intersects(g, b))
        goto cleanup;

    pieces_placed = 1;

    /* Main game loop - AI only mode */
    while (pieces_placed < MAX_PIECES) {
        /* Direct AI decision: get best move */
        move_t *best = best_move(g, b, ss, w);

        if (!best) /* No valid move found, natural game over */
            break;

        /* Validate the AI's suggested move */
        block_t test_block = *b;
        test_block.rot = best->rot;
        test_block.offset.x = best->col;

        /* Check if the suggested position is valid */
        if (grid_block_intersects(g, &test_block))
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
        if (grid_block_intersects(g, b))
            break;

        /* Add block to grid */
        grid_block_add(g, b);

        /* Clear completed lines */
        int cleared = grid_clear_lines(g);
        if (cleared > 0) {
            total_lines_cleared += cleared;

            /* Calculate points (standard Tetris scoring) */
            int line_points = cleared * 100;
            if (cleared > 1)
                line_points *= cleared; /* Bonus for multiple lines */
            total_points += line_points;
        }

        /* Generate next block */
        shape_stream_pop(ss);
        block_init(b, shape_stream_peek(ss, 0));
        grid_block_center_elevate(g, b);

        /* Check for game over */
        if (grid_block_intersects(g, b))
            break;

        pieces_placed++;

        /* Update progress bar periodically */
        if (pieces_placed % PROGRESS_UPDATE_INTERVAL == 0) {
            /* Calculate current total progress across all games */
            int current_total =
                (total_pieces_so_far ? *total_pieces_so_far : 0) +
                pieces_placed;

            /* Simple progress bar update - overwrite the existing line */
            printf("\r\x1b[K"); /* Clear current line */
            printf("Progress: [");

            /* Calculate filled characters */
            int filled =
                (current_total * progress_width) / total_expected_pieces;

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

    nfree(ss);
    nfree(g);
    nfree(b);

    return stats;
}

/* Run benchmark with multiple games */
bench_results_t bench_run(float *weights, int num_games)
{
    bench_results_t results = {0};

    if (num_games <= 0) {
        return results;
    }

    results.games = malloc(num_games * sizeof(game_stats_t));
    if (!results.games) {
        return results;
    }

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
        5000; /* Should match MAX_PIECES in bench_play_single */
    const int total_expected_pieces = num_games * max_pieces_per_game;

    printf("Progress: [");
    for (int i = 0; i < progress_width; i++)
        printf(" ");
    printf("] 0/%d pieces", total_expected_pieces);

    /* Run games with progress reporting */
    for (int i = 0; i < num_games; i++) {
        results.games[i] =
            bench_play_single(weights, &total_pieces, total_expected_pieces);

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

        /* Final progress bar update after each game */
        printf("\r\x1b[K"); /* Clear current line */
        printf("Progress: [");

        /* Calculate filled characters */
        int filled = (total_pieces * progress_width) / total_expected_pieces;

        /* Draw progress bar with simple blocks */
        printf("\x1b[32m"); /* Green color */
        for (int i = 0; i < filled; i++)
            printf("█");
        printf("\x1b[0m"); /* Reset color */
        for (int i = filled; i < progress_width; i++)
            printf(" ");

        printf("] %d/%d pieces", total_pieces, total_expected_pieces);
        fflush(stdout);

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
void bench_print_results(const bench_results_t *results)
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

void auto_play(float *w)
{
    grid_t *g = grid_new(GRID_HEIGHT, GRID_WIDTH);
    block_t *b = block_new();
    shape_stream_t *ss = shape_stream_new();

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
    block_init(b, shape_stream_peek(ss, 0));
    grid_block_center_elevate(g, b);

    /* Validate first block placement */
    if (grid_block_intersects(g, b)) {
        tui_show_falling_pieces(g);
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
            tui_block_print_preview(preview_block, preview_color);
        }
        nfree(preview_block);
    }

    /* Start timer AFTER first block is ready and displayed */
    init_timer();

    /* Force immediate display buffer build and render for first block */
    tui_build_display_buffer(g, b);
    tui_render_display_buffer(g);
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
                    if (!is_ai_mode)
                        init_timer();
                }
            }

            /* Resume normal rendering after unpause */
            tui_build_display_buffer(g, b);
            tui_render_display_buffer(g);
            tui_refresh();
            continue;
        }

        if (dropped) {
            /* Generate next block */
            shape_stream_pop(ss);
            block_init(b, shape_stream_peek(ss, 0));
            grid_block_center_elevate(g, b);

            /* Check for game over */
            if (grid_block_intersects(g, b))
                break;

            /* Always update preview after generating new block */
            block_t *preview_block = block_new();
            if (preview_block) {
                shape_t *next_shape = shape_stream_peek(ss, 1);
                if (next_shape) {
                    block_init(preview_block, next_shape);
                    int preview_color = tui_get_shape_color(next_shape);
                    tui_block_print_preview(preview_block, preview_color);
                } else {
                    /* Clear preview if no next shape available */
                    tui_block_print_preview(NULL, 0);
                }
                nfree(preview_block);
            } else {
                /* Clear preview if preview block creation failed */
                tui_block_print_preview(NULL, 0);
            }

            dropped = false;

            /* Force display buffer refresh when new block appears */
            tui_force_display_buffer_refresh();
        }

        /* Always rebuild and render each frame */

        /* Step 1: Always build complete display state (grid + falling block) */
        tui_build_display_buffer(g, b);

        /* Step 2: Always render from display buffer */
        tui_render_display_buffer(g);

        /* Step 3: Get input after rendering is complete */
        input_t input = tui_scankey();

        /* Handle global commands */
        switch (input) {
        case INPUT_TOGGLE_MODE: {
            is_ai_mode = !is_ai_mode;
            /* Force one clean redraw after mode switch */
            tui_force_redraw(g);
            tui_update_mode_display(is_ai_mode);
            if (is_ai_mode) {
                stop_timer();
            } else {
                init_timer();
            }
            /* Rebuild display after mode switch */
            tui_build_display_buffer(g, b);
            tui_render_display_buffer(g);
            continue; /* Skip movement this frame */
        }
        case INPUT_PAUSE: {
            is_paused = true;
            stop_timer();
            tui_prompt(g, "Paused - Press 'p' to resume");
            continue; /* Skip movement this frame */
        }
        case INPUT_QUIT:
            goto cleanup;
        default:
            break;
        }

        /* Handle auto-drop */
        if (!is_ai_mode && auto_drop_ready) {
            auto_drop_ready = 0;

            /* Use consistent movement function with validation */
            grid_block_move(g, b, BOT, 1);
            if (grid_block_intersects(g, b)) {
                /* Can't move down, revert and mark for placement */
                grid_block_move(g, b, TOP, 1);
                dropped = true;
            }
        }

        /* Handle movement input with consistent collision detection */
        if (!dropped) {
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
            /* Add piece to grid with validation */
            if (!grid_block_intersects(g, b)) {
                /* Get color BEFORE adding block to grid */
                int current_color = tui_get_shape_color(b->shape);

                /* Add block to grid */
                grid_block_add(g, b);

                /* Preserve the color assignment */
                tui_add_block_color(b, current_color);

                /* Prepare color preservation before checking for lines */
                tui_prepare_color_preservation(g);

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
                    tui_build_display_buffer(g, NULL);
                    tui_render_display_buffer(g);
                    tui_refresh();

                    /* Now show line clearing animation */
                    tui_flash_completed_lines(g, completed_rows, num_completed);
                }

                /* Clear lines using grid function */
                int cleared = grid_clear_lines(g);

                if (cleared > 0) {
                    /* Apply color preservation after line clearing */
                    tui_apply_color_preservation(g);

                    /* Force complete cleanup after line clearing */
                    tui_force_redraw(g);

                    /* Update statistics */
                    total_lines_cleared += cleared;
                    int line_points = cleared * 100;
                    if (cleared > 1)
                        line_points *= cleared;
                    total_points += line_points;
                    current_level = 1 + (total_lines_cleared / 10);

                    /* Update UI after statistics change */
                    tui_update_stats(current_level, total_points,
                                     total_lines_cleared);
                    tui_update_mode_display(is_ai_mode);
                } else {
                    /* No lines cleared, just force display buffer refresh */
                    tui_force_display_buffer_refresh();
                }
            }
        }

        /* Periodic maintenance and ensure preview stays visible */
        move_count++;
        if (move_count % 200 ==
            0) { /* Reduced frequency to prevent flickering */
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
                    tui_block_print_preview(refresh_preview, preview_color);
                }
                nfree(refresh_preview);
            }
        }

        /* Use very reduced periodic cleanup to prevent artifacts without
         * flickering
         */
        if (move_count % 1000 == 0) /* only every 1000 frames */
            tui_periodic_cleanup(g);

        /* Always refresh after each frame to ensure display is current */
        tui_refresh();

        /* Minimal delay to prevent CPU spinning */
        if (is_ai_mode) {
            /* AI already has delay from thinking time, but add tiny delay to
             * prevent busy wait.
             */
            usleep(0.01 * SECOND);
        } else {
            /* Minimal delay for responsive human play */
            usleep(0.01 * SECOND);
        }
    }

    tui_show_falling_pieces(g);
    tui_prompt(g, "Game Over!");
    sleep(3);
cleanup:
    stop_timer();
    tui_quit();
    nfree(ss);
    nfree(g);
    nfree(b);
    free_shape();
}
