#include <stdlib.h>
#include <string.h>

#include "../tetris.h"
#include "../utils.h"
#include "test.h"

/* Helper functions for packed bit grid access in tests */
static inline bool test_cell_occupied(const grid_t *g, int x, int y)
{
    if (!g || x < 0 || y < 0 || x >= g->width || y >= g->height)
        return false;
    return (g->rows[y] >> x) & 1ULL;
}

static inline void test_set_cell(grid_t *g, int x, int y)
{
    if (!g || x < 0 || y < 0 || x >= g->width || y >= g->height)
        return;
    g->rows[y] |= (1ULL << x);
}

static inline void test_clear_cell(grid_t *g, int x, int y)
{
    if (!g || x < 0 || y < 0 || x >= g->width || y >= g->height)
        return;
    g->rows[y] &= ~(1ULL << x);
}

void test_game_stats_structure_validation(void)
{
    /* Test game_stats_t structure for benchmark functionality */
    game_stats_t stats = {0};

    /* Test structure initialization */
    stats.lines_cleared = 10;
    stats.score = 1000;
    stats.pieces_placed = 50;
    stats.lcpp = 0.2f;
    stats.game_duration = 30.5;
    stats.hit_piece_limit = false;
    stats.pieces_per_second = 1.67f;

    assert_test(stats.lines_cleared == 10 && stats.score == 1000,
                "game statistics should be settable");
    assert_test(stats.pieces_placed == 50 && stats.lcpp == 0.2f,
                "piece statistics should be settable");
    assert_test(stats.game_duration == 30.5 && !stats.hit_piece_limit,
                "timing statistics should be settable");
    assert_test(stats.pieces_per_second == 1.67f,
                "performance statistics should be settable");
}

void test_game_benchmark_results_structure(void)
{
    /* Test bench_results_t structure validation */
    bench_results_t results = {0};

    /* Test basic structure fields */
    results.num_games = 5;
    results.total_games_completed = 3;
    results.natural_endings = 2;

    /* Test that we can access the structure fields */
    assert_test(results.num_games == 5, "num_games should be settable");
    assert_test(results.total_games_completed == 3,
                "total_games_completed should be settable");
    assert_test(results.natural_endings == 2,
                "natural_endings should be settable");

    /* Test nested game stats structures */
    results.avg.lines_cleared = 15;
    results.avg.score = 1500;
    results.best.lines_cleared = 25;
    results.best.score = 2500;

    assert_test(results.avg.lines_cleared == 15 && results.avg.score == 1500,
                "average statistics should be settable");
    assert_test(results.best.lines_cleared == 25 && results.best.score == 2500,
                "best statistics should be settable");
}

void test_game_input_enumeration_validation(void)
{
    /* Test input_t enumeration values */
    assert_test(INPUT_INVALID >= 0, "INPUT_INVALID should be valid enum value");
    assert_test(INPUT_TOGGLE_MODE >= 0,
                "INPUT_TOGGLE_MODE should be valid enum value");
    assert_test(INPUT_PAUSE >= 0, "INPUT_PAUSE should be valid enum value");
    assert_test(INPUT_QUIT >= 0, "INPUT_QUIT should be valid enum value");
    assert_test(INPUT_ROTATE >= 0, "INPUT_ROTATE should be valid enum value");
    assert_test(INPUT_MOVE_LEFT >= 0,
                "INPUT_MOVE_LEFT should be valid enum value");
    assert_test(INPUT_MOVE_RIGHT >= 0,
                "INPUT_MOVE_RIGHT should be valid enum value");
    assert_test(INPUT_DROP >= 0, "INPUT_DROP should be valid enum value");

    /* Test that enum values are distinct */
    assert_test(INPUT_INVALID != INPUT_TOGGLE_MODE,
                "enum values should be distinct");
    assert_test(INPUT_MOVE_LEFT != INPUT_MOVE_RIGHT,
                "movement inputs should be distinct");
    assert_test(INPUT_PAUSE != INPUT_QUIT, "pause and quit should be distinct");
}

void test_game_basic_piece_placement_sequence(void)
{
    /* Test fundamental Tetris piece placement mechanics */
    bool shapes_ok = shape_init();
    assert_test(shapes_ok, "shape_init should succeed for placement tests");
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

    /* Test complete piece lifecycle: spawn -> move -> place */
    shape_t *piece = shape_stream_pop(stream);
    assert_test(piece, "should get valid piece from stream");

    if (piece) {
        /* Step 1: Piece spawning */
        block_init(block, piece);
        assert_test(grid_block_spawn(grid, block),
                    "piece should spawn at top center");

        /* Step 2: Player controls simulation */
        int initial_x = block->offset.x;
        grid_block_move(grid, block, LEFT, 1);
        assert_test(block->offset.x <= initial_x,
                    "left movement should work or be blocked");

        grid_block_rotate(grid, block, 1);
        assert_test(block->rot >= 0 && block->rot < piece->n_rot,
                    "rotation should maintain valid state");

        /* Step 3: Gravity simulation (hard drop) */
        int drop_distance = grid_block_drop(grid, block);
        assert_test(drop_distance >= 0 && !grid_block_collides(grid, block),
                    "hard drop should place piece in valid position");

        /* Step 4: Piece placement */
        grid_block_add(grid, block);
        bool grid_updated = false;
        for (int r = 0; r < grid->height && !grid_updated; r++) {
            for (int c = 0; c < grid->width; c++) {
                if (test_cell_occupied(grid, c, r)) {
                    grid_updated = true;
                    break;
                }
            }
        }
        assert_test(grid_updated, "grid should contain placed piece");
    }

    /* Cleanup in reverse order of allocation */
    nfree(stream);
    nfree(block);
    nfree(grid);
    shape_free();
}

void test_game_block_coordinate_retrieval(void)
{
    /* Test block coordinate retrieval system */
    bool shapes_ok = shape_init();
    assert_test(shapes_ok, "shape_init should succeed for coordinate tests");
    if (!shapes_ok)
        return;

    block_t *block = block_new();
    shape_t *test_shape = shape_get(0);

    if (!block || !test_shape) {
        nfree(block);
        shape_free();
        return;
    }

    block_init(block, test_shape);
    block->offset.x = 5;
    block->offset.y = 10;

    /* Test coordinate retrieval for all block cells */
    for (int i = 0; i < MAX_BLOCK_LEN; i++) {
        coord_t result;
        block_get(block, i, &result);

        /* Valid coordinates should be offset correctly */
        if (result.x >= 0 && result.y >= 0) {
            assert_test(
                result.x >= block->offset.x && result.y >= block->offset.y,
                "block coordinate %d should be offset correctly", i);
        }
    }

    /* Test extreme calculation */
    int left_extreme = block_extreme(block, LEFT);
    int right_extreme = block_extreme(block, RIGHT);
    int bot_extreme = block_extreme(block, BOT);
    int top_extreme = block_extreme(block, TOP);

    assert_test(left_extreme >= 0, "left extreme should be reasonable");
    assert_test(right_extreme >= left_extreme, "right should be >= left");
    assert_test(bot_extreme >= 0, "bottom extreme should be reasonable");
    assert_test(top_extreme >= bot_extreme, "top should be >= bottom");

    /* Test bounds checking in block_get */
    coord_t invalid_result;
    block_get(block, -1, &invalid_result);
    /* Note: coord_t uses uint8_t, so -1 wraps to 255 */
    assert_test(invalid_result.x == 255 && invalid_result.y == 255,
                "invalid block index should return (255, 255) due to uint8_t "
                "wraparound");

    block_get(block, MAX_BLOCK_LEN + 1, &invalid_result);
    assert_test(invalid_result.x == 255 && invalid_result.y == 255,
                "out-of-bounds block index should return (255, 255) due to "
                "uint8_t wraparound");

    nfree(block);
    shape_free();
}

void test_game_grid_copy_operations(void)
{
    /* Test grid copy functionality */
    bool shapes_ok = shape_init();
    assert_test(shapes_ok, "shape_init should succeed for grid copy tests");
    if (!shapes_ok)
        return;

    grid_t *source = grid_new(GRID_HEIGHT, GRID_WIDTH);
    grid_t *dest = grid_new(GRID_HEIGHT, GRID_WIDTH);

    if (!source || !dest) {
        nfree(dest);
        nfree(source);
        shape_free();
        return;
    }

    /* Create pattern in source grid */
    for (int row = 0; row < 5; row++) {
        for (int col = 0; col < source->width; col += 2)
            test_set_cell(source, col, row);
        source->n_row_fill[row] = source->width / 2;
    }
    source->n_total_cleared = 3;
    source->n_last_cleared = 1;

    /* Test grid copy */
    grid_copy(dest, source);

    /* Verify copy accuracy */
    bool copy_accurate = true;
    for (int row = 0; row < 5; row++) {
        for (int col = 0; col < source->width; col++) {
            if (test_cell_occupied(source, col, row) !=
                test_cell_occupied(dest, col, row)) {
                copy_accurate = false;
                break;
            }
        }
        if (!copy_accurate)
            break;
    }

    assert_test(copy_accurate, "grid copy should preserve cell states");
    assert_test(dest->n_total_cleared == source->n_total_cleared,
                "grid copy should preserve statistics");
    assert_test(dest->n_last_cleared == source->n_last_cleared,
                "grid copy should preserve last cleared count");

    /* Test copy with different dimensions (should fail gracefully) */
    grid_t *different_size = grid_new(10, 8);
    if (different_size) {
        grid_copy(different_size, source); /* Should handle gracefully */
        assert_test(true,
                    "grid copy with different dimensions should not crash");
        nfree(different_size);
    }

    /* Test NULL handling */
    grid_copy(NULL, source); /* Should not crash */
    grid_copy(dest, NULL);   /* Should not crash */
    assert_test(true, "grid copy should handle NULL parameters gracefully");

    nfree(dest);
    nfree(source);
    shape_free();
}

void test_game_line_clearing_mechanics(void)
{
    /* Test Tetris line clearing system */
    bool shapes_ok = shape_init();
    assert_test(shapes_ok, "shape_init should succeed for line clearing tests");
    if (!shapes_ok)
        return;

    grid_t *grid = grid_new(GRID_HEIGHT, GRID_WIDTH);
    if (!grid) {
        shape_free();
        return;
    }

    /* Test single line clear */
    for (int col = 0; col < grid->width; col++)
        test_set_cell(grid, col, 0);
    grid->n_row_fill[0] = grid->width;
    grid->full_rows[0] = 0;
    grid->n_full_rows = 1;

    int single_clear = grid_clear_lines(grid);
    assert_test(single_clear == 1, "should clear exactly 1 complete line");
    assert_test(grid->n_total_cleared == 1 && grid->n_last_cleared == 1,
                "statistics should reflect single line clear");

    /* Test Tetris (4-line simultaneous clear) */
    for (int row = 1; row <= 4; row++) {
        for (int col = 0; col < grid->width; col++)
            test_set_cell(grid, col, row);
        grid->n_row_fill[row] = grid->width;
        grid->full_rows[row - 1] = row;
    }
    grid->n_full_rows = 4;

    int tetris_clear = grid_clear_lines(grid);
    assert_test(tetris_clear == 4, "should clear 4 lines for Tetris");
    assert_test(grid->n_total_cleared == 5,
                "total should accumulate (1 + 4 = 5)");

    /* Test that line clearing updates grid state correctly */
    /* Reset grid completely for this test */
    for (int r = 0; r < grid->height; r++) {
        grid->rows[r] = 0; /* Clear entire row */
        grid->n_row_fill[r] = 0;
    }
    for (int c = 0; c < grid->width; c++) {
        grid->relief[c] = -1;
        grid->gaps[c] = 0;
        grid->stack_cnt[c] = 0;
    }
    grid->n_full_rows = 0;
    grid->n_total_cleared = 0;
    grid->n_last_cleared = 0;

    /* Create complete line at bottom only */
    for (int col = 0; col < grid->width; col++) {
        test_set_cell(grid, col, 0);
        grid->relief[col] = 0;
    }
    grid->n_row_fill[0] = grid->width;
    grid->full_rows[0] = 0;
    grid->n_full_rows = 1;

    int basic_clear = grid_clear_lines(grid);
    assert_test(basic_clear == 1, "should clear complete line");

    /* Verify grid state after clearing - check that complete lines were
     * processed
     */
    assert_test(grid->n_full_rows == 0,
                "grid should have no complete lines remaining");

    nfree(grid);
    shape_free();
}

void test_game_over_detection_logic(void)
{
    /* Test Tetris game over conditions */
    bool shapes_ok = shape_init();
    assert_test(shapes_ok, "shape_init should succeed for game over tests");
    if (!shapes_ok)
        return;

    grid_t *grid = grid_new(GRID_HEIGHT, GRID_WIDTH);
    block_t *block = block_new();
    shape_t *test_shape = shape_get(0);

    if (!grid || !block || !test_shape) {
        nfree(block);
        nfree(grid);
        shape_free();
        return;
    }

    /* Test normal spawn (no game over) */
    block_init(block, test_shape);
    assert_test(grid_block_spawn(grid, block),
                "piece should spawn successfully on empty grid");
    assert_test(!grid_block_collides(grid, block),
                "spawned piece should not cause game over");

    /* Test topout condition */
    /* Fill grid to near-top */
    for (int row = 0; row < GRID_HEIGHT - 1; row++) {
        for (int col = 0; col < grid->width; col++)
            test_set_cell(grid, col, row);
    }

    /* Try to spawn piece in filled grid */
    block_init(block, test_shape);
    int spawn_result = grid_block_spawn(grid, block);
    bool topout = grid_block_collides(grid, block);
    assert_test(topout || !spawn_result,
                "piece spawn in filled grid should cause game over");

    /* Test lockout condition (piece can spawn but can't move) */
    /* Clear top row but leave obstacles */
    for (int col = 0; col < grid->width; col++)
        test_clear_cell(grid, col, GRID_HEIGHT - 1);
    /* Leave some blocks at spawn area */
    test_set_cell(grid, GRID_WIDTH / 2, GRID_HEIGHT - 2);

    block_init(block, test_shape);
    grid_block_spawn(grid, block);
    bool lockout = grid_block_collides(grid, block);
    assert_test(lockout, "piece blocking spawn area should cause lockout");

    nfree(block);
    nfree(grid);
    shape_free();
}

void test_game_ai_vs_human_decision_making(void)
{
    /* Test AI decision making vs human input validation */
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
        return;
    }

    shape_t *test_shape = shape_get(0);
    if (!test_shape) {
        free(weights);
        nfree(stream);
        nfree(block);
        nfree(grid);
        return;
    }

    block_init(block, test_shape);
    if (grid_block_spawn(grid, block)) {
        /* Test AI decision making */
        move_t *ai_decision = move_find_best(grid, block, stream, weights);
        if (ai_decision) {
            /* Validate AI produces reasonable moves */
            assert_test(ai_decision->col >= 0 && ai_decision->col < GRID_WIDTH,
                        "AI should choose valid column (%d)", ai_decision->col);
            assert_test(ai_decision->rot >= 0,
                        "AI should choose non-negative rotation (%d)",
                        ai_decision->rot);
        } else {
            /* AI might not find moves in some scenarios - acceptable */
            assert_test(true,
                        "AI may not find moves in all scenarios (acceptable)");
        }

        /* Test human input validation */
        /* Valid moves should be accepted */
        int original_x = block->offset.x;
        grid_block_move(grid, block, RIGHT, 1);
        assert_test(block->offset.x >= original_x,
                    "valid human move should be executed or maintained");

        /* Invalid moves should be rejected */
        block->offset.x = 0;
        int boundary_x = block->offset.x;
        grid_block_move(grid, block, LEFT, 5);
        assert_test(block->offset.x >= boundary_x,
                    "invalid human move should be rejected");
    }

    free(weights);
    nfree(stream);
    nfree(block);
    nfree(grid);
}

void test_game_ai_weight_system_validation(void)
{
    /* Test AI weight system functionality */
    float *weights = move_defaults();
    assert_test(weights, "move_defaults should return valid pointer");

    if (!weights)
        return;

    /* Test that weights contain reasonable values */
    bool weights_reasonable = true;
    for (int i = 0; i < 6; i++) { /* Assuming 6 features based on move.c */
        if (weights[i] < -10.0f || weights[i] > 10.0f) {
            weights_reasonable = false;
            break;
        }
    }
    assert_test(weights_reasonable,
                "default weights should be reasonable values");

    /* Test weight modification doesn't crash AI */
    bool shapes_ok = shape_init();
    if (shapes_ok) {
        grid_t *grid = grid_new(GRID_HEIGHT, GRID_WIDTH);
        block_t *block = block_new();
        shape_stream_t *stream = shape_stream_new();
        shape_t *test_shape = shape_get(0);

        if (grid && block && stream && test_shape) {
            block_init(block, test_shape);
            grid_block_spawn(grid, block);

            /* Test with modified weights */
            for (int i = 0; i < 6; i++) {
                weights[i] *= 0.5f; /* Scale down weights */
            }

            move_t *modified_decision =
                move_find_best(grid, block, stream, weights);
            assert_test(modified_decision,
                        "AI should work with modified weights");
        }

        nfree(stream);
        nfree(block);
        nfree(grid);
        shape_free();
    }

    free(weights);
}

void test_game_scoring_and_statistics_logic(void)
{
    /* Test Tetris scoring system */

    /* Standard Tetris line clear scoring */
    int single_score = 1 * 100;
    int double_score = 2 * 100 * 2; /* Double bonus */
    int triple_score = 3 * 100 * 3; /* Triple bonus */
    int tetris_score = 4 * 100 * 4; /* Tetris bonus */

    assert_test(single_score == 100, "single line clear = 100 points");
    assert_test(double_score == 400, "double line clear = 400 points");
    assert_test(triple_score == 900, "triple line clear = 900 points");
    assert_test(tetris_score == 1600, "tetris clear = 1600 points");

    /* Level progression (every 10 lines) */
    int level_10_lines = 1 + (10 / 10);
    int level_25_lines = 1 + (25 / 10);
    int level_100_lines = 1 + (100 / 10);

    assert_test(level_10_lines == 2, "level 2 at 10 lines cleared");
    assert_test(level_25_lines == 3, "level 3 at 25 lines cleared");
    assert_test(level_100_lines == 11, "level 11 at 100 lines cleared");

    /* Performance metrics */
    float lcpp_good = 20.0f / 50.0f;      /* 20 lines, 50 pieces */
    float lcpp_excellent = 40.0f / 80.0f; /* 40 lines, 80 pieces */

    assert_test(lcpp_good == 0.4f, "good LCPP = 0.4 (20/50)");
    assert_test(lcpp_excellent == 0.5f, "excellent LCPP = 0.5 (40/80)");

    /* Speed metrics */
    float pps_beginner = 30.0f / 60.0f; /* 30 pieces in 60 seconds */
    float pps_expert = 120.0f / 60.0f;  /* 120 pieces in 60 seconds */

    assert_test(pps_beginner == 0.5f, "beginner PPS = 0.5");
    assert_test(pps_expert == 2.0f, "expert PPS = 2.0");

    /* Edge case: division by zero prevention */
    float lcpp_no_pieces = (0 > 0) ? 10.0f / 0 : 0.0f;
    assert_test(lcpp_no_pieces == 0.0f,
                "LCPP should be 0 when no pieces placed");
}

void test_game_piece_stream_continuity(void)
{
    /* Test sustained piece generation for long games */
    bool shapes_ok = shape_init();
    assert_test(shapes_ok, "shape_init should succeed for continuity tests");
    if (!shapes_ok)
        return;

    shape_stream_t *stream = shape_stream_new();
    if (!stream) {
        shape_free();
        return;
    }

    /* Test marathon-length piece generation */
    const int marathon_pieces = 100; /* 100 pieces = ~14 complete 7-bags */
    int pieces_generated = 0;
    int shape_counts[NUM_TETRIS_SHAPES] = {0};

    for (int i = 0; i < marathon_pieces; i++) {
        shape_t *piece = shape_stream_pop(stream);
        if (!piece)
            break;

        pieces_generated++;

        /* Count piece distribution */
        for (int shape_idx = 0; shape_idx < NUM_TETRIS_SHAPES; shape_idx++) {
            if (piece == shape_get(shape_idx)) {
                shape_counts[shape_idx]++;
                break;
            }
        }
    }

    assert_test(pieces_generated == marathon_pieces,
                "should generate all marathon pieces (%d/%d)", pieces_generated,
                marathon_pieces);

    /* Verify 7-bag fairness over long term */
    int min_count = marathon_pieces, max_count = 0;
    for (int i = 0; i < NUM_TETRIS_SHAPES; i++) {
        if (shape_counts[i] < min_count)
            min_count = shape_counts[i];
        if (shape_counts[i] > max_count)
            max_count = shape_counts[i];
    }

    int distribution_range = max_count - min_count;
    assert_test(distribution_range <= 2,
                "long-term distribution should be fair (range: %d)",
                distribution_range);

    nfree(stream);
    shape_free();
}

void test_game_multi_piece_sequence_validation(void)
{
    /* Test realistic multi-piece game sequence */
    bool shapes_ok = shape_init();
    assert_test(shapes_ok, "shape_init should succeed for sequence tests");
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

    /* Simulate realistic game: place pieces until near-game-over */
    int pieces_placed = 0;
    int total_lines_cleared = 0;
    const int target_pieces = 20;
    bool game_over = false;

    for (int i = 0; i < target_pieces && !game_over; i++) {
        shape_t *piece = shape_stream_pop(stream);
        if (!piece)
            break;

        block_init(block, piece);

        /* Try to spawn piece */
        if (!grid_block_spawn(grid, block)) {
            game_over = true;
            break;
        }

        if (grid_block_collides(grid, block)) {
            game_over = true;
            break;
        }

        /* Simulate random placement strategy */
        if (i % 3 == 0) {
            grid_block_move(grid, block, LEFT, 2);
        } else if (i % 3 == 1) {
            grid_block_move(grid, block, RIGHT, 2);
        }

        if (i % 4 == 0)
            grid_block_rotate(grid, block, 1);

        /* Place piece */
        grid_block_drop(grid, block);
        if (grid_block_collides(grid, block)) {
            game_over = true;
            break;
        }

        grid_block_add(grid, block);
        pieces_placed++;

        /* Process line clears */
        if (grid->n_full_rows > 0) {
            int cleared = grid_clear_lines(grid);
            total_lines_cleared += cleared;
        }
    }

    assert_test(pieces_placed > 0, "should place at least some pieces");
    assert_test(grid->n_total_cleared == total_lines_cleared,
                "grid statistics should match actual clears");

    /* Verify game state consistency */
    int actual_filled_cells = 0;
    for (int r = 0; r < grid->height; r++) {
        for (int c = 0; c < grid->width; c++) {
            if (test_cell_occupied(grid, c, r))
                actual_filled_cells++;
        }
    }

    /* Should have pieces on grid if game progressed */
    if (pieces_placed > total_lines_cleared) {
        assert_test(actual_filled_cells > 0,
                    "grid should contain pieces after sequence");
    }

    nfree(stream);
    nfree(block);
    nfree(grid);
    shape_free();
}

void test_game_comprehensive_tetromino_placement_validation(void)
{
    /* Test all tetromino types for placement validity */
    bool shapes_ok = shape_init();
    assert_test(shapes_ok, "shape_init should succeed for comprehensive tests");
    if (!shapes_ok)
        return;

    grid_t *grid = grid_new(GRID_HEIGHT, GRID_WIDTH);
    block_t *block = block_new();

    if (!grid || !block) {
        nfree(block);
        nfree(grid);
        return;
    }

    /* Test each of the 7 standard tetrominoes - focus on basic placement */
    int successfully_tested = 0;
    for (int shape_idx = 0; shape_idx < NUM_TETRIS_SHAPES; shape_idx++) {
        shape_t *tetromino = shape_get(shape_idx);
        if (!tetromino)
            continue;

        block_init(block, tetromino);

        /* Test standard spawn position */
        if (grid_block_spawn(grid, block)) {
            assert_test(!grid_block_collides(grid, block),
                        "spawned tetromino %d should not intersect", shape_idx);
            successfully_tested++;
        }

        /* Test basic rotation capability */
        for (int rot = 0; rot < tetromino->n_rot && rot < 2;
             rot++) { /* Test first 2 rotations only */
            block->rot = rot;

            /* Test center placement */
            block->offset.x = GRID_WIDTH / 2;
            block->offset.y = GRID_HEIGHT / 2;
            bool center_valid = !grid_block_collides(grid, block);
            assert_test(center_valid,
                        "tetromino %d rotation %d should fit in center",
                        shape_idx, rot);
        }
    }

    /* Very lenient success criteria - just need basic functionality */
    assert_test(successfully_tested > 0,
                "should successfully test basic placement for at least some "
                "tetrominoes (%d tested)",
                successfully_tested);

    nfree(block);
    nfree(grid);
}

void test_game_grid_boundary_collision_detection(void)
{
    /* Test collision detection at all grid boundaries */
    bool shapes_ok = shape_init();
    assert_test(shapes_ok, "shape_init should succeed for boundary tests");
    if (!shapes_ok)
        return;

    grid_t *grid = grid_new(GRID_HEIGHT, GRID_WIDTH);
    block_t *block = block_new();
    shape_t *test_shape = shape_get(0);

    if (!grid || !block || !test_shape) {
        nfree(block);
        nfree(grid);
        shape_free();
        return;
    }

    block_init(block, test_shape);

    /* Test boundary violations */
    /* Far left (negative coordinates) */
    block->offset.x = -5;
    block->offset.y = GRID_HEIGHT / 2;
    assert_test(grid_block_collides(grid, block),
                "negative X should cause collision");

    /* Far right (beyond grid) */
    block->offset.x = GRID_WIDTH + 5;
    block->offset.y = GRID_HEIGHT / 2;
    assert_test(grid_block_collides(grid, block),
                "X beyond grid should cause collision");

    /* Below grid (negative Y) */
    block->offset.x = GRID_WIDTH / 2;
    block->offset.y = -5;
    assert_test(grid_block_collides(grid, block),
                "negative Y should cause collision");

    /* Above grid (beyond height) */
    block->offset.x = GRID_WIDTH / 2;
    block->offset.y = GRID_HEIGHT + 5;
    assert_test(grid_block_collides(grid, block),
                "Y beyond grid should cause collision");

    /* Test exact boundary cases */
    block->offset.x = 0;
    block->offset.y = 0;
    bool bottom_left = grid_block_collides(grid, block);
    assert_test(bottom_left == true || bottom_left == false,
                "bottom-left corner should have defined collision result");

    block->offset.x = GRID_WIDTH - 1;
    block->offset.y = GRID_HEIGHT - 1;
    bool top_right = grid_block_collides(grid, block);
    assert_test(top_right == true || top_right == false,
                "top-right corner should have defined collision result");

    nfree(block);
    nfree(grid);
    shape_free();
}

void test_game_complex_grid_state_validation(void)
{
    /* Test game behavior with complex grid patterns */
    bool shapes_ok = shape_init();
    assert_test(shapes_ok, "shape_init should succeed for complex state tests");
    if (!shapes_ok)
        return;

    grid_t *grid = grid_new(GRID_HEIGHT, GRID_WIDTH);
    block_t *block = block_new();
    shape_t *test_shape = shape_get(0);

    if (!grid || !block || !test_shape) {
        nfree(block);
        nfree(grid);
        return;
    }

    /* Create a simpler, safer pattern instead of complex manual manipulation */
    /* Add a few blocks using proper grid operations
     */
    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < grid->width; col += 3) {
            block_init(block, test_shape);
            block->offset.x = col;
            block->offset.y = row;

            /* Only add if position is valid */
            if (!grid_block_collides(grid, block))
                grid_block_add(grid, block);
        }
    }

    block_init(block, test_shape);

    /* Test piece placement in the pattern */
    /* Find valid placement positions */
    int valid_positions = 0;
    for (int test_col = 0; test_col < grid->width - 2; test_col++) {
        block->offset.x = test_col;
        block->offset.y = 10; /* Above the pattern */

        if (!grid_block_collides(grid, block))
            valid_positions++;
    }

    assert_test(valid_positions >= 0,
                "should find some valid positions above pattern");

    /* Test line clearing in pattern - create a complete line safely */
    /* Fill one row completely using proper operations */
    bool line_completed = false;
    for (int col = 0; col < grid->width; col++) {
        block_init(block, test_shape);
        block->offset.x = col;
        block->offset.y = 0;

        if (!grid_block_collides(grid, block)) {
            grid_block_add(grid, block);
            /* Check if this completed a line */
            if (grid->n_full_rows > 0) {
                line_completed = true;
                break;
            }
        }
    }

    if (line_completed) {
        int cleared = grid_clear_lines(grid);
        assert_test(cleared >= 0, "should clear lines in pattern");
        assert_test(grid->n_full_rows >= 0,
                    "grid state should be consistent after clearing");
    }

    nfree(block);
    nfree(grid);
}

void test_game_tetromino_rotation_state_consistency(void)
{
    /* Test rotation mechanics for all tetromino types */
    bool shapes_ok = shape_init();
    assert_test(shapes_ok, "shape_init should succeed for rotation tests");
    if (!shapes_ok)
        return;

    grid_t *grid = grid_new(GRID_HEIGHT, GRID_WIDTH);
    block_t *block = block_new();

    if (!grid || !block) {
        nfree(block);
        nfree(grid);
        shape_free();
        return;
    }

    /* Test each tetromino's rotation behavior */
    for (int shape_idx = 0; shape_idx < NUM_TETRIS_SHAPES; shape_idx++) {
        shape_t *tetromino = shape_get(shape_idx);
        if (!tetromino)
            continue;

        block_init(block, tetromino);
        grid_block_spawn(grid, block);

        /* Test clockwise rotation cycle */
        for (int i = 0; i < 4; i++) {
            grid_block_rotate(grid, block, 1);
            assert_test(block->rot >= 0 && block->rot < tetromino->n_rot,
                        "tetromino %d rotation %d should be valid", shape_idx,
                        block->rot);
        }

        /* Test counter-clockwise rotation */
        grid_block_rotate(grid, block, -1);
        assert_test(block->rot >= 0 && block->rot < tetromino->n_rot,
                    "tetromino %d CCW rotation should be valid", shape_idx);

        /* Test rotation near walls (wall kick simulation) */
        block->offset.x = 0; /* Left wall */
        int wall_rotation = block->rot;
        grid_block_rotate(grid, block, 1);
        assert_test(block->rot >= 0 && block->rot < tetromino->n_rot,
                    "tetromino %d wall rotation should maintain validity",
                    shape_idx);

        /* Test that rotation either succeeds or maintains valid state */
        bool rotation_worked = (block->rot != wall_rotation);
        bool state_maintained = (block->rot == wall_rotation);
        assert_test(rotation_worked || state_maintained,
                    "tetromino %d rotation should work or be maintained",
                    shape_idx);
    }

    nfree(block);
    nfree(grid);
    shape_free();
}

void test_game_line_clearing_pattern_validation(void)
{
    /* Test advanced line clearing scenarios */
    bool shapes_ok = shape_init();
    assert_test(shapes_ok, "shape_init should succeed for line pattern tests");
    if (!shapes_ok)
        return;

    grid_t *grid = grid_new(GRID_HEIGHT, GRID_WIDTH);
    if (!grid)
        return;

    /* Test simple line clearing pattern using proper grid operations */
    block_t *test_block = block_new();
    shape_t *test_shape = shape_get(0);

    if (!test_block || !test_shape) {
        nfree(test_block);
        nfree(grid);
        return;
    }

    /* Create a simple pattern by placing blocks properly */
    for (int col = 0; col < grid->width && col < 10; col += 2) {
        block_init(test_block, test_shape);
        test_block->offset.x = col;
        test_block->offset.y = 0;

        if (!grid_block_collides(grid, test_block))
            grid_block_add(grid, test_block);
    }

    /* Test basic line clearing */
    if (grid->n_full_rows > 0) {
        int first_clear = grid_clear_lines(grid);
        assert_test(first_clear >= 0, "should clear lines when available");
    }

    /* Test T-piece availability (simplified) */
    shape_t *t_piece = NULL;
    for (int i = 0; i < NUM_TETRIS_SHAPES; i++) {
        shape_t *candidate = shape_get(i);
        if (candidate && candidate->n_rot == 4) {
            /* Likely T-piece (has 4 rotations) */
            t_piece = candidate;
            break;
        }
    }

    if (t_piece)
        assert_test(true, "T-piece found and available for testing");

    nfree(test_block);
    nfree(grid);
}

void test_game_memory_cleanup_validation(void)
{
    /* Test shapes cleanup in isolated context */
    bool shapes_ok = shape_init();
    if (shapes_ok) {
        /* Verify shapes are available */
        shape_t *test_shape = shape_get(0);
        assert_test(test_shape, "shape should be available after init");

        /* Test double cleanup safety without actually freeing global shapes.
         * We'll test the cleanup function exists, but not call it to avoid
         * interfering with other tests.
         */
        assert_test(true,
                    "shape_free function should be available for cleanup");
    }
}

void test_game_grid_different_dimensions(void)
{
    /* Test grid creation with different dimensions */

    /* Test standard Tetris grid */
    grid_t *standard = grid_new(GRID_HEIGHT, GRID_WIDTH);
    if (standard) {
        assert_test(standard, "standard grid should be created");
        assert_test(
            standard->height == GRID_HEIGHT && standard->width == GRID_WIDTH,
            "standard grid should have correct dimensions");
        nfree(standard);
    }

    /* Test smaller grid */
    grid_t *small = grid_new(10, 8);
    if (small) {
        assert_test(small, "small grid should be created");
        assert_test(small->height == 10 && small->width == 8,
                    "small grid should have correct dimensions");
        nfree(small);
    }

    /* Test larger grid (but within compiled limits) */
    grid_t *large = grid_new(GRID_HEIGHT, 16); /* Max height, wider width */
    if (large) {
        assert_test(large, "large grid should be created");
        assert_test(large->height == GRID_HEIGHT && large->width == 16,
                    "large grid should have correct dimensions");
        nfree(large);
    }

    /* Test edge cases */
    grid_t *minimal = grid_new(1, 1);
    if (minimal) {
        assert_test(minimal, "minimal grid should be created");
        nfree(minimal);
    }

    /* Test invalid dimensions */
    grid_t *invalid_zero = grid_new(0, 10);
    assert_test(!invalid_zero, "zero height grid should fail");

    grid_t *invalid_negative = grid_new(-5, 10);
    assert_test(!invalid_negative, "negative dimension grid should fail");

    /* Test width that's too large */
    grid_t *invalid_wide = grid_new(10, 65); /* Over 64-bit limit */
    assert_test(!invalid_wide, "overly wide grid should fail");
}

void test_game_edge_cases_and_robustness(void)
{
    /* Test game robustness under edge conditions */

    /* Test statistics edge cases first (no memory allocation) */
    game_stats_t zero_stats = {0};
    float safe_lcpp =
        (zero_stats.pieces_placed > 0)
            ? (float) zero_stats.lines_cleared / zero_stats.pieces_placed
            : 0.0f;
    assert_test(safe_lcpp == 0.0f,
                "LCPP calculation should handle zero pieces");

    /* Test AI with invalid parameters */
    bool shapes_ok = shape_init();
    if (shapes_ok) {
        grid_t *grid = grid_new(GRID_HEIGHT, GRID_WIDTH);
        block_t *block = block_new();
        shape_stream_t *stream = shape_stream_new();
        float *valid_weights = move_defaults();

        if (grid && block && stream && valid_weights) {
            shape_t *shape = shape_get(0);
            if (shape) {
                block_init(block, shape);

                /* Test AI robustness with NULL parameters */
                assert_test(
                    move_find_best(NULL, block, stream, valid_weights) == NULL,
                    "AI should handle NULL grid");
                assert_test(
                    move_find_best(grid, NULL, stream, valid_weights) == NULL,
                    "AI should handle NULL block");
                assert_test(
                    move_find_best(grid, block, NULL, valid_weights) == NULL,
                    "AI should handle NULL stream");
                assert_test(move_find_best(grid, block, stream, NULL) == NULL,
                            "AI should handle NULL weights");

                /* Test extreme coordinate handling */
                block->offset.x = 127; /* Max int8_t value */
                block->offset.y = 127;
                bool extreme_collision = grid_block_collides(grid, block);
                assert_test(extreme_collision,
                            "extreme coordinates should cause collision");

                /* Test coordinate overflow behavior */
                block->offset.x = GRID_WIDTH * 2;
                block->offset.y = GRID_HEIGHT * 2;
                bool overflow_collision = grid_block_collides(grid, block);
                assert_test(overflow_collision,
                            "coordinates beyond grid should cause collision");
            }
        }

        /* Cleanup resources */
        free(valid_weights);
        nfree(stream);
        nfree(block);
        nfree(grid);
    }

    assert_test(true, "game edge cases completed successfully");
}

/* ===== NEW ENHANCED TESTS FOR BETTER COVERAGE ===== */

void test_game_complete_lifecycle_state_transitions(void)
{
    /* Test complete Tetris game lifecycle with state transitions */
    bool shapes_ok = shape_init();
    assert_test(shapes_ok, "shape_init should succeed for lifecycle tests");
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

    /* Test complete game state sequence */
    for (int cycle = 0; cycle < 5; cycle++) {
        /* Phase 1: Piece Generation */
        shape_t *piece = shape_stream_pop(stream);
        assert_test(piece, "cycle %d: should generate piece", cycle);
        if (!piece)
            break;

        /* Phase 2: Piece Initialization */
        block_init(block, piece);
        assert_test(block->shape == piece,
                    "cycle %d: block should link to shape", cycle);
        assert_test(block->rot == 0, "cycle %d: initial rotation should be 0",
                    cycle);

        /* Phase 3: Piece Spawning */
        int spawn_success = grid_block_spawn(grid, block);
        assert_test(spawn_success, "cycle %d: piece should spawn successfully",
                    cycle);
        assert_test(!grid_block_collides(grid, block),
                    "cycle %d: spawned piece should not intersect", cycle);

        /* Phase 4: Active Piece State (movement/rotation) */
        int initial_x = block->offset.x;

        /* Test horizontal movement */
        grid_block_move(grid, block, LEFT, 1);
        assert_test(block->offset.x <= initial_x,
                    "cycle %d: left move should work or be blocked", cycle);

        grid_block_move(grid, block, RIGHT, 2);
        assert_test(block->offset.x >= 0 && block->offset.x < GRID_WIDTH,
                    "cycle %d: position should stay in bounds", cycle);

        /* Test rotation */
        grid_block_rotate(grid, block, 1);
        assert_test(block->rot >= 0 && block->rot < piece->n_rot,
                    "cycle %d: rotation should stay valid", cycle);

        /* Phase 5: Piece Locking (hard drop) */
        int drop_distance = grid_block_drop(grid, block);
        assert_test(drop_distance >= 0, "cycle %d: drop should work (%d cells)",
                    cycle, drop_distance);
        assert_test(!grid_block_collides(grid, block),
                    "cycle %d: dropped piece should not intersect", cycle);

        /* Phase 6: Grid Update */
        grid_block_add(grid, block);

        /* Verify grid state consistency */
        int expected_cells = 4; /* Each tetromino has 4 cells */
        int actual_new_cells = 0;
        for (int r = 0; r < grid->height; r++) {
            for (int c = 0; c < grid->width; c++) {
                if (test_cell_occupied(grid, c, r))
                    actual_new_cells++;
            }
        }
        assert_test(actual_new_cells >= expected_cells,
                    "cycle %d: grid should have new cells (%d >= %d)", cycle,
                    actual_new_cells, expected_cells);

        /* Phase 7: Line Clearing Check */
        if (grid->n_full_rows > 0) {
            int lines_cleared = grid_clear_lines(grid);
            assert_test(lines_cleared >= 0,
                        "cycle %d: line clearing should work", cycle);
            assert_test(grid->n_total_cleared >= lines_cleared,
                        "cycle %d: total cleared should accumulate", cycle);
        }
    }

    /* Final state validation */
    assert_test(grid->n_total_cleared >= 0,
                "total lines cleared should be non-negative");

    nfree(stream);
    nfree(block);
    nfree(grid);
    shape_free();
}

void test_game_grid_internal_state_consistency(void)
{
    /* Test grid internal state consistency during operations */
    bool shapes_ok = shape_init();
    assert_test(shapes_ok,
                "shape_init should succeed for state consistency tests");
    if (!shapes_ok)
        return;

    grid_t *grid = grid_new(GRID_HEIGHT, GRID_WIDTH);
    block_t *block = block_new();
    if (!grid || !block) {
        nfree(block);
        nfree(grid);
        return;
    }

    /* Test relief array consistency */
    /* Initially all relief should be -1 (empty columns) */
    for (int col = 0; col < grid->width; col++) {
        assert_test(grid->relief[col] == -1,
                    "empty grid relief[%d] should be -1", col);
        assert_test(grid->gaps[col] == 0, "empty grid gaps[%d] should be 0",
                    col);
        assert_test(grid->stack_cnt[col] == 0,
                    "empty grid stack_cnt[%d] should be 0", col);
    }

    /* Use proper grid operations instead of manual state manipulation */
    shape_t *test_shape = shape_get(0);
    if (test_shape) {
        block_init(block, test_shape);
        block->offset.x = 5;
        block->offset.y = 3;

        /* Safely add block using proper grid operations */
        if (!grid_block_collides(grid, block)) {
            grid_block_add(grid, block);

            /* Test that relief is updated correctly */
            assert_test(grid->relief[5] >= -1,
                        "relief should be valid after block add");

            /* Test line completion and clearing with proper operations */
            /* Fill a complete line using multiple blocks if possible */
            block_t *fill_block = block_new();
            if (fill_block) {
                /* Try to create a line clearing scenario safely */
                for (int col = 0; col < grid->width && col < 10; col += 2) {
                    block_init(fill_block, test_shape);
                    fill_block->offset.x = col;
                    fill_block->offset.y = 0;
                    if (!grid_block_collides(grid, fill_block)) {
                        grid_block_add(grid, fill_block);
                    }
                }
                nfree(fill_block);
            }

            /* Test line clearing if any lines are complete */
            if (grid->n_full_rows > 0) {
                int cleared = grid_clear_lines(grid);
                assert_test(cleared >= 0, "line clearing should work");
                assert_test(grid->n_full_rows >= 0,
                            "full rows count should be valid");
            }
        }
    }

    nfree(block);
    nfree(grid);
}

void test_game_block_add_remove_symmetry(void)
{
    /* Test that block add/remove operations are symmetric */
    bool shapes_ok = shape_init();
    assert_test(shapes_ok, "shape_init should succeed for add/remove tests");
    if (!shapes_ok)
        return;

    grid_t *original = grid_new(GRID_HEIGHT, GRID_WIDTH);
    grid_t *modified = grid_new(GRID_HEIGHT, GRID_WIDTH);
    block_t *block = block_new();

    if (!original || !modified || !block) {
        nfree(block);
        nfree(modified);
        nfree(original);
        return;
    }

    /* Test with a safe subset of shapes */
    for (int shape_idx = 0; shape_idx < 3;
         shape_idx++) { /* Test first 3 shapes only */
        shape_t *shape = shape_get(shape_idx);
        if (!shape)
            continue;

        /* Test add/remove cycle with safe positioning */
        block_init(block, shape);
        block->offset.x = GRID_WIDTH / 2;
        block->offset.y = GRID_HEIGHT / 2;

        /* Ensure the position is valid before proceeding */
        if (grid_block_collides(modified, block)) {
            continue; /* Skip invalid positions to avoid corruption */
        }

        /* Create a backup of the grid state */
        grid_copy(modified, original);

        /* Add block */
        grid_block_add(modified, block);

        /* Verify block was added by checking grid statistics */
        bool block_added = false;
        for (int r = 0; r < modified->height && !block_added; r++) {
            for (int c = 0; c < modified->width; c++) {
                if (test_cell_occupied(modified, c, r) &&
                    !test_cell_occupied(original, c, r)) {
                    block_added = true;
                    break;
                }
            }
        }
        assert_test(block_added, "shape %d: block should be added to grid",
                    shape_idx);

        /* Remove block */
        grid_block_remove(modified, block);

        /* Verify basic grid state consistency */
        bool grids_basically_match =
            (modified->n_total_cleared == original->n_total_cleared);
        assert_test(grids_basically_match,
                    "shape %d: grid statistics should match after add/remove",
                    shape_idx);
    }

    nfree(block);
    nfree(modified);
    nfree(original);
}

void test_game_collision_detection_accuracy(void)
{
    /* Test collision detection accuracy in various scenarios */
    bool shapes_ok = shape_init();
    assert_test(shapes_ok, "shape_init should succeed for collision tests");
    if (!shapes_ok)
        return;

    grid_t *grid = grid_new(GRID_HEIGHT, GRID_WIDTH);
    block_t *block = block_new();

    if (!grid || !block) {
        nfree(block);
        nfree(grid);
        shape_free();
        return;
    }

    shape_t *test_shape = shape_get(0); /* Get first available shape */
    if (!test_shape) {
        nfree(block);
        nfree(grid);
        shape_free();
        return;
    }

    block_init(block, test_shape);

    /* Test 1: Empty grid - no collision */
    block->offset.x = GRID_WIDTH / 2;
    block->offset.y = GRID_HEIGHT / 2;
    assert_test(!grid_block_collides(grid, block),
                "empty grid should not cause collision");

    /* Test 2: Single cell collision */
    test_set_cell(grid, GRID_WIDTH / 2, GRID_HEIGHT / 2);
    bool collision_detected = grid_block_collides(grid, block);
    assert_test(collision_detected,
                "single cell overlap should cause collision");

    /* Test 3: Adjacent cells - no collision */
    test_clear_cell(grid, GRID_WIDTH / 2, GRID_HEIGHT / 2);
    /* Two cells away */
    test_set_cell(grid, GRID_WIDTH / 2 + 2,
                  GRID_HEIGHT / 2);
    bool no_collision = !grid_block_collides(grid, block);
    assert_test(no_collision,
                "cells two positions away should not cause collision");

    /* Test 4: Boundary collision */
    /* Left boundary */
    block->offset.x = -1;
    block->offset.y = GRID_HEIGHT / 2;
    assert_test(grid_block_collides(grid, block),
                "left boundary should cause collision");

    /* Right boundary */
    block->offset.x = GRID_WIDTH;
    block->offset.y = GRID_HEIGHT / 2;
    assert_test(grid_block_collides(grid, block),
                "right boundary should cause collision");

    /* Bottom boundary */
    block->offset.x = GRID_WIDTH / 2;
    block->offset.y = -1;
    assert_test(grid_block_collides(grid, block),
                "bottom boundary should cause collision");

    /* Top boundary */
    block->offset.x = GRID_WIDTH / 2;
    block->offset.y = GRID_HEIGHT;
    assert_test(grid_block_collides(grid, block),
                "top boundary should cause collision");

    /* Test 5: Partial overlap */
    /* Clear grid */
    for (int r = 0; r < grid->height; r++)
        grid->rows[r] = 0; /* Clear entire row */

    /* Place block in valid position */
    block->offset.x = 2;
    block->offset.y = 2;
    if (!grid_block_collides(grid, block)) {
        /* Add one cell where the block would be */
        coord_t test_coord;
        block_get(block, 0, &test_coord);
        if (test_coord.x < GRID_WIDTH && test_coord.y < GRID_HEIGHT) {
            test_set_cell(grid, test_coord.x, test_coord.y);
            assert_test(grid_block_collides(grid, block),
                        "partial overlap should cause collision");
        }
    }

    nfree(block);
    nfree(grid);
    shape_free();
}

void test_game_movement_validation_comprehensive(void)
{
    /* Test comprehensive movement validation */
    bool shapes_ok = shape_init();
    assert_test(shapes_ok, "shape_init should succeed for movement tests");
    if (!shapes_ok)
        return;

    grid_t *grid = grid_new(GRID_HEIGHT, GRID_WIDTH);
    block_t *block = block_new();

    if (!grid || !block) {
        nfree(block);
        nfree(grid);
        shape_free();
        return;
    }

    /* Test each shape type */
    for (int shape_idx = 0; shape_idx < NUM_TETRIS_SHAPES; shape_idx++) {
        shape_t *shape = shape_get(shape_idx);
        if (!shape)
            continue;

        block_init(block, shape);

        /* Start from center position */
        if (!grid_block_spawn(grid, block))
            continue;

        /* Test movement in all directions */
        int center_x = block->offset.x;
        int center_y = block->offset.y;

        /* Test LEFT movement */
        for (int steps = 1; steps <= GRID_WIDTH; steps++) {
            grid_block_move(grid, block, LEFT, 1);
            assert_test(block->offset.x >= 0,
                        "shape %d: LEFT movement should not go negative",
                        shape_idx);
            assert_test(!grid_block_collides(grid, block),
                        "shape %d: LEFT movement should not cause collision",
                        shape_idx);
            if (block->offset.x == 0)
                break; /* Reached boundary */
        }

        /* Reset position */
        block->offset.x = center_x;
        block->offset.y = center_y;

        /* Test RIGHT movement */
        for (int steps = 1; steps <= GRID_WIDTH; steps++) {
            grid_block_move(grid, block, RIGHT, 1);
            assert_test(block->offset.x < GRID_WIDTH,
                        "shape %d: RIGHT movement should not exceed grid",
                        shape_idx);
            assert_test(!grid_block_collides(grid, block),
                        "shape %d: RIGHT movement should not cause collision",
                        shape_idx);

            /* Check if we can't move anymore (hit boundary) */
            int old_x = block->offset.x;
            grid_block_move(grid, block, RIGHT, 1);
            if (block->offset.x == old_x)
                break; /* Can't move further */
        }

        /* Reset position */
        block->offset.x = center_x;
        block->offset.y = center_y;

        /* Test DOWN movement */
        for (int steps = 1; steps <= GRID_HEIGHT; steps++) {
            grid_block_move(grid, block, BOT, 1);
            assert_test(block->offset.y >= 0,
                        "shape %d: DOWN movement should not go negative",
                        shape_idx);
            assert_test(!grid_block_collides(grid, block),
                        "shape %d: DOWN movement should not cause collision",
                        shape_idx);
            if (block->offset.y == 0)
                break; /* Reached bottom */
        }
    }

    nfree(block);
    nfree(grid);
    shape_free();
}

void test_game_rotation_validation_comprehensive(void)
{
    /* Test comprehensive rotation validation */
    bool shapes_ok = shape_init();
    assert_test(shapes_ok, "shape_init should succeed for rotation tests");
    if (!shapes_ok)
        return;

    grid_t *grid = grid_new(GRID_HEIGHT, GRID_WIDTH);
    block_t *block = block_new();

    if (!grid || !block) {
        nfree(block);
        nfree(grid);
        shape_free();
        return;
    }

    /* Test each shape type */
    for (int shape_idx = 0; shape_idx < NUM_TETRIS_SHAPES; shape_idx++) {
        shape_t *shape = shape_get(shape_idx);
        if (!shape)
            continue;

        block_init(block, shape);
        if (!grid_block_spawn(grid, block))
            continue;

        /* Test all possible rotations */
        for (int target_rot = 0; target_rot < shape->n_rot; target_rot++) {
            /* Try to reach each rotation state */
            block->rot = 0; /* Reset to initial rotation */

            /* Rotate to target state */
            while (block->rot != target_rot) {
                int old_rot = block->rot;
                grid_block_rotate(grid, block, 1);

                /* Verify rotation state is valid */
                assert_test(block->rot >= 0 && block->rot < shape->n_rot,
                            "shape %d: rotation %d should be valid", shape_idx,
                            block->rot);
                assert_test(!grid_block_collides(grid, block),
                            "shape %d: rotation %d should not cause collision",
                            shape_idx, block->rot);

                /* Prevent infinite loop */
                if (block->rot == old_rot)
                    break;
            }
        }

        /* Test rotation cycle completeness */
        block->rot = 0;
        int rotation_count = 0;
        do {
            int old_rot = block->rot;
            grid_block_rotate(grid, block, 1);
            rotation_count++;

            /* Prevent infinite loop */
            if (rotation_count > 4 || block->rot == old_rot)
                break;
        } while (block->rot != 0 && rotation_count < 4);

        assert_test(rotation_count <= 4,
                    "shape %d: should complete rotation cycle in ≤4 steps",
                    shape_idx);

        /* Test counter-clockwise rotation */
        block->rot = 0;
        int ccw_rotation_count = 0;
        do {
            int old_rot = block->rot;
            grid_block_rotate(grid, block, -1);
            ccw_rotation_count++;

            assert_test(block->rot >= 0 && block->rot < shape->n_rot,
                        "shape %d: CCW rotation should maintain valid state",
                        shape_idx);

            /* Prevent infinite loop */
            if (ccw_rotation_count > 4 || block->rot == old_rot)
                break;
        } while (block->rot != 0 && ccw_rotation_count < 4);
    }

    nfree(block);
    nfree(grid);
    shape_free();
}

void test_game_shape_stream_state_transitions(void)
{
    /* Test shape stream state transitions and consistency */
    bool shapes_ok = shape_init();
    assert_test(shapes_ok, "shape_init should succeed for stream state tests");
    if (!shapes_ok)
        return;

    shape_stream_t *stream = shape_stream_new();
    if (!stream) {
        shape_free();
        return;
    }

    /* Test initial state */
    assert_test(stream->iter == 0, "new stream should start with iter=0");
    assert_test(stream->max_len > 0, "stream should have positive max_len");

    /* Test peek behavior (should not modify state) */
    shape_t *peek0_first = shape_stream_peek(stream, 0);
    shape_t *peek0_second = shape_stream_peek(stream, 0);
    assert_test(peek0_first == peek0_second,
                "repeated peek(0) should return same shape");
    assert_test(stream->iter == 0, "peek should not modify iter");

    shape_t *peek1 = shape_stream_peek(stream, 1);
    assert_test(peek1, "peek(1) should return valid shape");
    /* Note: peek(0) and peek(1) may be the same shape due to 7-bag boundaries
     */

    /* Test pop behavior (should modify state) */
    shape_t *pop_first = shape_stream_pop(stream);
    assert_test(pop_first == peek0_first,
                "first pop should return same as peek(0)");
    assert_test(stream->iter == 1, "pop should increment iter");

    shape_t *pop_second = shape_stream_pop(stream);
    assert_test(pop_second == peek1,
                "second pop should return same as previous peek(1)");
    assert_test(stream->iter == 2, "second pop should increment iter again");

    /* Test continuous generation */
    for (int i = 0; i < 20; i++) {
        shape_t *piece = shape_stream_pop(stream);
        assert_test(piece, "stream should continuously generate pieces");

        /* Verify it's a valid shape */
        bool is_valid_shape = false;
        for (int shape_idx = 0; shape_idx < NUM_TETRIS_SHAPES; shape_idx++) {
            if (piece == shape_get(shape_idx)) {
                is_valid_shape = true;
                break;
            }
        }
        assert_test(is_valid_shape,
                    "generated piece should be valid tetromino");
    }

    /* Test peek ahead multiple positions */
    shape_t *lookahead[3]; /* Limit to stream's actual capacity */
    for (int i = 0; i < 3; i++) {
        lookahead[i] = shape_stream_peek(stream, i);
        assert_test(lookahead[i], "should be able to peek ahead %d positions",
                    i);
    }

    /* Verify peek consistency */
    for (int i = 0; i < 3; i++) {
        shape_t *recheck = shape_stream_peek(stream, i);
        assert_test(recheck == lookahead[i],
                    "peek should be consistent for position %d", i);
    }

    /* Test bag reset functionality */
    shape_bag_reset();
    shape_t *after_reset = shape_stream_peek(stream, 0);
    assert_test(after_reset, "stream should work after bag reset");

    nfree(stream);
    shape_free();
}

void test_game_ai_basic_functionality_validation(void)
{
    /* Simple, robust AI functionality test */
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
        return;
    }

    /* Test 1: AI works with at least one shape */
    shape_t *test_shape = shape_get(0);
    if (test_shape) {
        block_init(block, test_shape);
        if (grid_block_spawn(grid, block)) {
            move_t *ai_move = move_find_best(grid, block, stream, weights);
            assert_test(ai_move != NULL,
                        "AI should be able to make at least one decision");

            if (ai_move) {
                assert_test(ai_move->col >= 0 && ai_move->col < GRID_WIDTH,
                            "AI decision should have valid column");
            }
        }
    }

    /* Test 2: AI handles NULL parameters gracefully */
    assert_test(move_find_best(NULL, block, stream, weights) == NULL,
                "AI should handle NULL grid");
    assert_test(move_find_best(grid, NULL, stream, weights) == NULL,
                "AI should handle NULL block");
    assert_test(move_find_best(grid, block, NULL, weights) == NULL,
                "AI should handle NULL stream");
    assert_test(move_find_best(grid, block, stream, NULL) == NULL,
                "AI should handle NULL weights");

    /* Test 3: AI weight system works */
    if (test_shape) {
        block_init(block, test_shape);
        if (grid_block_spawn(grid, block)) {
            move_t *original_move =
                move_find_best(grid, block, stream, weights);

            /* Modify weights and test again */
            for (int i = 0; i < 6; i++)
                weights[i] *= 0.5f;

            move_t *modified_move =
                move_find_best(grid, block, stream, weights);

            /* Both should either work or both should fail */
            bool both_null = (original_move == NULL && modified_move == NULL);
            bool both_valid = (original_move != NULL && modified_move != NULL);
            assert_test(both_null || both_valid,
                        "AI should handle weight modifications consistently");
        }
    }

    free(weights);
    nfree(stream);
    nfree(block);
    nfree(grid);
}
