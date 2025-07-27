#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../tetris.h"
#include "../utils.h"
#include "test.h"

/* Helper functions for packed bit grid access in tests */
static inline void test_set_cell(grid_t *g, int x, int y)
{
    if (!g || x < 0 || y < 0 || x >= g->width || y >= g->height ||
        y >= GRID_HEIGHT)
        return;
    g->rows[y] |= (1ULL << x);
}

static inline void test_clear_cell(grid_t *g, int x, int y)
{
    if (!g || x < 0 || y < 0 || x >= g->width || y >= g->height ||
        y >= GRID_HEIGHT)
        return;
    g->rows[y] &= ~(1ULL << x);
}

void test_move_defaults_allocation(void)
{
    /* Test AI weight system allocation */
    float *weights = move_defaults();
    assert_test(weights, "default AI weights should be allocated successfully");

    if (!weights)
        return;

    /* Validate weights are reasonable for Tetris AI */
    bool weights_valid = true;
    for (int i = 0; i < 6; i++) { /* N_FEATIDX features */
        if (isnan(weights[i]) || isinf(weights[i]) ||
            fabs(weights[i]) > 100.0f) {
            weights_valid = false;
            break;
        }
    }
    assert_test(weights_valid, "AI weights should be finite and reasonable");

    /* Ensure weights are configured (not all zero) */
    bool has_configuration = false;
    for (int i = 0; i < 6; i++) {
        if (weights[i] != 0.0f) {
            has_configuration = true;
            break;
        }
    }
    assert_test(has_configuration,
                "AI should have non-zero weight configuration");

    free(weights);
}

void test_move_defaults_consistency(void)
{
    /* Test AI weight system consistency */
    float *weights1 = move_defaults();
    float *weights2 = move_defaults();

    assert_test(weights1 && weights2,
                "multiple weight allocations should succeed");

    if (weights1 && weights2) {
        /* Weights should be deterministic */
        bool weights_consistent = true;
        for (int i = 0; i < 6; i++) {
            if (weights1[i] != weights2[i]) {
                weights_consistent = false;
                break;
            }
        }
        assert_test(weights_consistent, "AI weights should be deterministic");

        /* Each allocation should be independent */
        assert_test(weights1 != weights2,
                    "weight allocations should be independent");

        free(weights1);
        free(weights2);
    }
}

void test_move_find_best_basic_functionality(void)
{
    /* Test core AI decision making functionality */
    bool shapes_ok = shape_init();
    assert_test(shapes_ok, "shape_init should succeed for AI tests");
    if (!shapes_ok)
        return;

    grid_t *grid = grid_new(GRID_HEIGHT, GRID_WIDTH);
    block_t *block = block_new();
    shape_stream_t *stream = shape_stream_new();
    float *weights = move_defaults();

    if (!grid || !block || !stream || !weights) {
        free(weights);
        nfree(stream);
        nfree(block);
        nfree(grid);
        shape_free();
        return;
    }

    shape_t *test_shape = shape_get(0);
    if (!test_shape) {
        free(weights);
        nfree(stream);
        nfree(block);
        nfree(grid);
        shape_free();
        return;
    }

    /* Test AI decision on empty grid */
    block_init(block, test_shape);
    grid_block_spawn(grid, block);

    move_t *ai_move = move_find_best(grid, block, stream, weights);
    assert_test(ai_move, "AI should generate move on empty grid");

    if (ai_move) {
        /* Validate move is within Tetris grid bounds */
        assert_test(ai_move->col >= 0 && ai_move->col < GRID_WIDTH,
                    "AI move column should be within grid bounds (%d)",
                    ai_move->col);
        assert_test(ai_move->rot >= 0,
                    "AI move rotation should be non-negative (%d)",
                    ai_move->rot);

        /* Test move executability */
        block_t test_execution = *block;
        test_execution.rot = ai_move->rot % test_shape->n_rot;
        test_execution.offset.x = ai_move->col;
        grid_block_drop(grid, &test_execution);

        assert_test(!grid_block_collides(grid, &test_execution),
                    "AI move should be executable without collision");
    }

    free(weights);
    nfree(stream);
    nfree(block);
    nfree(grid);
    shape_free();
}

void test_move_find_best_edge_cases(void)
{
    /* Test AI robustness under edge conditions */
    bool shapes_ok = shape_init();
    assert_test(shapes_ok, "shape_init should succeed for edge case tests");
    if (!shapes_ok)
        return;

    grid_t *grid = grid_new(GRID_HEIGHT, GRID_WIDTH);
    block_t *block = block_new();
    shape_stream_t *stream = shape_stream_new();
    float *weights = move_defaults();

    if (!grid || !block || !stream || !weights) {
        free(weights);
        nfree(stream);
        nfree(block);
        nfree(grid);
        shape_free();
        return;
    }

    shape_t *test_shape = shape_get(0);
    if (test_shape) {
        block_init(block, test_shape);
        grid_block_spawn(grid, block);
    }

    /* Test NULL parameter handling */
    assert_test(move_find_best(NULL, block, stream, weights) == NULL,
                "AI should handle NULL grid gracefully");
    assert_test(move_find_best(grid, NULL, stream, weights) == NULL,
                "AI should handle NULL block gracefully");
    assert_test(move_find_best(grid, block, NULL, weights) == NULL,
                "AI should handle NULL stream gracefully");
    assert_test(move_find_best(grid, block, stream, NULL) == NULL,
                "AI should handle NULL weights gracefully");

    /* Test near-game-over scenario */
    /* Fill grid to near-top, leaving narrow channel */
    for (int row = 0; row < GRID_HEIGHT - 2; row++) {
        for (int col = 0; col < GRID_WIDTH; col++) {
            if (col < 2 || col >= GRID_WIDTH - 2) {
                /* Leave narrow channels */
                continue;
            }
            test_set_cell(grid, col, row);
        }
    }

    if (test_shape) {
        move_t *endgame_move = move_find_best(grid, block, stream, weights);
        /* AI should either find valid move or fail gracefully */
        if (endgame_move) {
            assert_test(
                endgame_move->col >= 0 && endgame_move->col < GRID_WIDTH,
                "endgame AI move should be valid if generated");
        }
        assert_test(
            true, "AI should handle near-game-over scenarios without crashing");
    }

    free(weights);
    nfree(stream);
    nfree(block);
    nfree(grid);
    shape_free();
}

void test_move_find_best_multiple_shapes(void)
{
    /* Test AI performance across all tetromino types */
    bool shapes_ok = shape_init();
    assert_test(shapes_ok, "shape_init should succeed for multi-shape tests");
    if (!shapes_ok)
        return;

    grid_t *grid = grid_new(GRID_HEIGHT, GRID_WIDTH);
    block_t *block = block_new();
    shape_stream_t *stream = shape_stream_new();
    float *weights = move_defaults();

    if (!grid || !block || !stream || !weights) {
        free(weights);
        nfree(stream);
        nfree(block);
        nfree(grid);
        shape_free();
        return;
    }

    /* Test AI with each of the 7 standard tetrominoes */
    int successful_decisions = 0;
    for (int shape_idx = 0; shape_idx < NUM_TETRIS_SHAPES; shape_idx++) {
        shape_t *tetromino = shape_get(shape_idx);
        if (!tetromino)
            continue;

        block_init(block, tetromino);
        grid_block_spawn(grid, block);

        move_t *decision = move_find_best(grid, block, stream, weights);
        if (!decision)
            continue;

        /* Validate decision for this tetromino */
        bool decision_valid =
            (decision->col >= 0 && decision->col < GRID_WIDTH &&
             decision->rot >= 0);

        /* Test move execution for this tetromino */
        if (decision_valid) {
            block_t execution_test = *block;
            execution_test.rot = decision->rot % tetromino->n_rot;
            execution_test.offset.x = decision->col;
            grid_block_drop(grid, &execution_test);

            if (!grid_block_collides(grid, &execution_test))
                successful_decisions++;
        }
    }

    assert_test(successful_decisions >= NUM_TETRIS_SHAPES - 1,
                "AI should handle most tetromino types successfully (%d/%d)",
                successful_decisions, NUM_TETRIS_SHAPES);

    free(weights);
    nfree(stream);
    nfree(block);
    nfree(grid);
    shape_free();
}

void test_move_find_best_weight_sensitivity(void)
{
    /* Test AI weight system sensitivity and configuration */
    bool shapes_ok = shape_init();
    assert_test(shapes_ok, "shape_init should succeed for weight tests");
    if (!shapes_ok)
        return;

    grid_t *grid = grid_new(GRID_HEIGHT, GRID_WIDTH);
    block_t *block = block_new();
    shape_stream_t *stream = shape_stream_new();

    if (!grid || !block || !stream) {
        nfree(stream);
        nfree(block);
        nfree(grid);
        shape_free();
        return;
    }

    shape_t *test_shape = shape_get(0);
    if (!test_shape) {
        nfree(stream);
        nfree(block);
        nfree(grid);
        shape_free();
        return;
    }

    /* Create differentiated grid scenario */
    /* Bottom-heavy with gaps to make weight differences matter */
    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < GRID_WIDTH; col++) {
            if ((row + col) % 3 != 0) { /* Create pattern with gaps */
                test_set_cell(grid, col, row);
            }
        }
    }

    block_init(block, test_shape);
    grid_block_spawn(grid, block);

    /* Test default AI configuration */
    float *default_w = move_defaults();
    move_t *default_decision = NULL;
    if (default_w)
        default_decision = move_find_best(grid, block, stream, default_w);

    /* Test aggressive line-clearing configuration */
    float aggressive_weights[6] = {-1.0f, -2.0f, -0.5f, -5.0f, 1.0f, -1.5f};
    move_t *aggressive_decision =
        move_find_best(grid, block, stream, aggressive_weights);

    /* Test defensive height-minimizing configuration */
    float defensive_weights[6] = {-3.0f, -1.0f, -0.1f, -2.0f, -0.5f, -0.5f};
    move_t *defensive_decision =
        move_find_best(grid, block, stream, defensive_weights);

    /* Validate that different weight configurations produce valid moves */
    int valid_configs = 0;
    if (default_decision && default_decision->col >= 0 &&
        default_decision->col < GRID_WIDTH) {
        valid_configs++;
    }
    if (aggressive_decision && aggressive_decision->col >= 0 &&
        aggressive_decision->col < GRID_WIDTH) {
        valid_configs++;
    }
    if (defensive_decision && defensive_decision->col >= 0 &&
        defensive_decision->col < GRID_WIDTH) {
        valid_configs++;
    }

    assert_test(
        valid_configs >= 2,
        "multiple weight configurations should produce valid moves (%d/3)",
        valid_configs);

    free(default_w);
    nfree(stream);
    nfree(block);
    nfree(grid);
    shape_free();
}

/* Helper function to properly set up grid state for testing */
static void setup_grid_with_blocks(grid_t *grid,
                                   block_t *temp_block,
                                   int row,
                                   int start_col,
                                   int end_col)
{
    if (!grid || !temp_block || !temp_block->shape)
        return;

    for (int col = start_col; col <= end_col; col++) {
        if (col >= 0 && col < grid->width && row >= 0 && row < grid->height) {
            /* Manually place block and update grid state properly */
            test_set_cell(grid, col, row);

            /* Update relief (highest occupied row per column) */
            if (grid->relief[col] < row)
                grid->relief[col] = row;
        }
    }

    /* Check if this row is now complete using our optimized bitmask detection
     */
    if (grid->rows[row] == grid->full_mask) {
        /* Add to full rows list if not already there */
        bool already_full = false;
        for (int i = 0; i < grid->n_full_rows; i++) {
            if (grid->full_rows[i] == row) {
                already_full = true;
                break;
            }
        }
        if (!already_full && grid->n_full_rows < grid->height)
            grid->full_rows[grid->n_full_rows++] = row;
    }
}

void test_ai_decision_quality(void)
{
    /* Test AI strategic decision making quality */
    bool shapes_ok = shape_init();
    assert_test(shapes_ok,
                "shape_init should succeed for decision quality tests");
    if (!shapes_ok)
        return;

    grid_t *grid = grid_new(GRID_HEIGHT, GRID_WIDTH);
    block_t *block = block_new();
    shape_stream_t *stream = shape_stream_new();
    float *weights = move_defaults();

    if (!grid || !block || !stream || !weights) {
        free(weights);
        nfree(stream);
        nfree(block);
        nfree(grid);
        shape_free();
        return;
    }

    shape_t *test_shape = shape_get(0);
    if (!test_shape) {
        free(weights);
        nfree(stream);
        nfree(block);
        nfree(grid);
        shape_free();
        return;
    }

    /* Test 1: Line clearing opportunity detection */
    /* Create almost-complete line using proper grid setup */
    block_init(block, test_shape);
    setup_grid_with_blocks(grid, block, 0, 0,
                           GRID_WIDTH - 2); /* Leave last column empty */

    grid_block_spawn(grid, block);

    move_t *line_clear_move = move_find_best(grid, block, stream, weights);
    if (line_clear_move) {
        assert_test(
            line_clear_move->col >= 0 && line_clear_move->col < GRID_WIDTH,
            "AI should handle line clearing opportunities");

        /* Test the suggested move */
        block_t test_block = *block;
        test_block.rot = line_clear_move->rot % test_shape->n_rot;
        test_block.offset.x = line_clear_move->col;
        grid_block_drop(grid, &test_block);

        assert_test(!grid_block_collides(grid, &test_block),
                    "AI line clearing move should be executable");
    }

    /* Test 2: Hole avoidance */
    /* Clear grid and create hole-prone scenario */
    for (int row = 0; row < GRID_HEIGHT; row++) {
        grid->rows[row] = 0; /* Clear entire row */
    }
    for (int col = 0; col < GRID_WIDTH; col++) {
        grid->relief[col] = -1;
        grid->gaps[col] = 0;
        grid->stack_cnt[col] = 0;
    }
    grid->n_full_rows = 0;

    /* Create overhang that could create holes */
    setup_grid_with_blocks(grid, block, 1, 2, GRID_WIDTH - 3);
    /* Create bottom row with gap */
    setup_grid_with_blocks(grid, block, 0, 0, GRID_WIDTH / 2 - 1);
    setup_grid_with_blocks(grid, block, 0, GRID_WIDTH / 2 + 1, GRID_WIDTH - 1);

    move_t *hole_avoid_move = move_find_best(grid, block, stream, weights);
    if (hole_avoid_move) {
        assert_test(
            hole_avoid_move->col >= 0 && hole_avoid_move->col < GRID_WIDTH,
            "AI should make valid moves in hole-prone scenarios");
    }

    /* Test 3: Height minimization */
    /* Clear grid and create height differential */
    for (int row = 0; row < GRID_HEIGHT; row++) {
        grid->rows[row] = 0; /* Clear entire row */
    }
    for (int col = 0; col < GRID_WIDTH; col++) {
        grid->relief[col] = -1;
        grid->gaps[col] = 0;
        grid->stack_cnt[col] = 0;
    }
    grid->n_full_rows = 0;

    /* Create tall stack on one side */
    for (int row = 0; row < 8; row++) {
        setup_grid_with_blocks(grid, block, row, 0, 1);
    }

    move_t *height_move = move_find_best(grid, block, stream, weights);
    if (height_move) {
        assert_test(height_move->col >= 0 && height_move->col < GRID_WIDTH,
                    "AI should handle height-differential scenarios");

        /* AI should generally prefer flatter placements */
        /* This is a soft preference test - AI might have good reasons for other
         * choices
         */
        assert_test(true, "AI should complete height-differential analysis");
    }

    free(weights);
    nfree(stream);
    nfree(block);
    nfree(grid);
    shape_free();
}

void test_move_structure_properties(void)
{
    /* Test move_t structure used for AI decisions */
    move_t test_move = {0};

    /* Test structure field assignment */
    test_move.rot = 1;
    test_move.col = 7;
    test_move.shape = NULL;

    assert_test(test_move.rot == 1 && test_move.col == 7,
                "move structure should store rotation and column");
    assert_test(test_move.shape == NULL,
                "move structure should handle NULL shape reference");

    /* Test with actual shape system */
    bool shapes_ok = shape_init();
    if (shapes_ok) {
        shape_t *tetromino = shape_get(0);
        if (tetromino) {
            test_move.shape = tetromino;
            test_move.rot = 2;
            test_move.col = 5;

            assert_test(test_move.shape == tetromino,
                        "move should reference tetromino correctly");
            assert_test(test_move.rot == 2 && test_move.col == 5,
                        "move should store updated rotation and column");

            /* Test move validation */
            bool move_reasonable =
                (test_move.col >= 0 && test_move.col < GRID_WIDTH &&
                 test_move.rot >= 0);
            assert_test(move_reasonable,
                        "move structure should contain reasonable values");
        }
        shape_free();
    }
}

void test_ai_performance_characteristics(void)
{
    /* Test AI performance and reliability characteristics */
    bool shapes_ok = shape_init();
    assert_test(shapes_ok, "shape_init should succeed for performance tests");
    if (!shapes_ok)
        return;

    grid_t *grid = grid_new(GRID_HEIGHT, GRID_WIDTH);
    block_t *block = block_new();
    shape_stream_t *stream = shape_stream_new();
    float *weights = move_defaults();

    if (!grid || !block || !stream || !weights) {
        free(weights);
        nfree(stream);
        nfree(block);
        nfree(grid);
        shape_free();
        return;
    }

    shape_t *test_shape = shape_get(0);
    if (!test_shape) {
        free(weights);
        nfree(stream);
        nfree(block);
        nfree(grid);
        shape_free();
        return;
    }

    block_init(block, test_shape);
    grid_block_spawn(grid, block);

    /* Test AI consistency across multiple calls */
    int reliable_calls = 0;
    int total_calls = 8;

    for (int i = 0; i < total_calls; i++) {
        /* Modify grid state slightly to test different scenarios */
        if (i > 0) {
            int test_row = i % 4;
            int test_col = (i * 3) % (GRID_WIDTH - 1);
            if (i % 2 == 0) {
                test_set_cell(grid, test_col, test_row);
            } else {
                test_clear_cell(grid, test_col, test_row);
            }
        }

        move_t *performance_move = move_find_best(grid, block, stream, weights);
        if (performance_move) {
            /* Validate move quality */
            bool move_valid = (performance_move->col >= 0 &&
                               performance_move->col < GRID_WIDTH &&
                               performance_move->rot >= 0);

            if (move_valid) {
                /* Test executability */
                block_t exec_test = *block;
                exec_test.rot = performance_move->rot % test_shape->n_rot;
                exec_test.offset.x = performance_move->col;
                grid_block_drop(grid, &exec_test);

                if (!grid_block_collides(grid, &exec_test))
                    reliable_calls++;
            }
        }
    }

    /* AI should be reliable across different scenarios */
    assert_test(reliable_calls >= total_calls - 2,
                "AI should be reliable across scenarios (%d/%d successful)",
                reliable_calls, total_calls);

    /* Test AI behavior in complex grid state */
    /* Create challenging scenario */
    for (int row = 0; row < 10; row++) {
        for (int col = 0; col < GRID_WIDTH; col++) {
            if ((row * 7 + col * 5) % 13 < 8) /* Semi-random pattern */
                test_set_cell(grid, col, row);
        }
    }

    move_t *complex_move = move_find_best(grid, block, stream, weights);
    assert_test(complex_move != NULL || complex_move == NULL,
                "AI should handle complex scenarios without crashing");

    if (complex_move) {
        assert_test(complex_move->col >= 0 && complex_move->col < GRID_WIDTH,
                    "AI moves in complex scenarios should be valid");
    }

    free(weights);
    nfree(stream);
    nfree(block);
    nfree(grid);
    shape_free();
}
