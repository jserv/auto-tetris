#include <stdlib.h>
#include <string.h>

#include "../nalloc.h"
#include "../tetris.h"
#include "test.h"

void test_shape_system_initialization(void)
{
    /* Test shapes_init functionality */
    bool init_result = shapes_init();
    assert_test(init_result, "shapes_init should succeed");

    if (!init_result)
        return;

    /* Test that we can get all 7 standard tetromino shapes */
    for (int i = 0; i < NUM_TETRIS_SHAPES; i++) {
        shape_t *shape = shape_get_by_index(i);
        assert_test(shape, "shape_get_by_index(%d) should return valid shape",
                    i);

        if (shape) {
            /* Validate basic tetromino properties */
            assert_test(shape->n_rot > 0 && shape->n_rot <= 4,
                        "tetromino %d should have 1-4 rotations", i);
            assert_test(shape->max_dim_len >= 2 && shape->max_dim_len <= 4,
                        "tetromino %d should fit in 2x2 to 4x4 bounding box",
                        i);
            assert_test(shape->rot[0],
                        "tetromino %d should have rotation 0 data", i);
        }
    }

    /* Test cleanup */
    shapes_free();
    assert_test(shape_get_by_index(0) == NULL,
                "shape_get_by_index should return NULL after shapes_free");
}

void test_shape_index_bounds_checking(void)
{
    bool init_result = shapes_init();
    assert_test(init_result, "shapes_init should succeed for bounds tests");

    if (!init_result)
        return;

    /* Test valid indices (standard 7 tetrominoes) */
    for (int i = 0; i < NUM_TETRIS_SHAPES; i++) {
        shape_t *shape = shape_get_by_index(i);
        assert_test(shape, "valid tetromino index %d should return shape", i);
    }

    /* Test invalid indices */
    assert_test(shape_get_by_index(-1) == NULL,
                "negative index should return NULL");
    assert_test(shape_get_by_index(NUM_TETRIS_SHAPES) == NULL,
                "index >= NUM_TETRIS_SHAPES should return NULL");
    assert_test(shape_get_by_index(1000) == NULL,
                "very large index should return NULL");

    /* Test boundary case */
    shape_t *boundary_shape = shape_get_by_index(NUM_TETRIS_SHAPES - 1);
    assert_test(boundary_shape, "boundary index should return valid shape");

    shapes_free();
}

void test_shape_properties_validation(void)
{
    bool init_result = shapes_init();
    assert_test(init_result, "shapes_init should succeed for property tests");

    if (!init_result)
        return;

    /* Test tetromino-specific properties */
    for (int i = 0; i < NUM_TETRIS_SHAPES; i++) {
        shape_t *shape = shape_get_by_index(i);
        if (!shape)
            continue;

        /* Validate rotation properties for Tetris gameplay */
        assert_test(shape->n_rot >= 1 && shape->n_rot <= 4,
                    "tetromino %d should have 1-4 unique rotations", i);

        /* Validate all rotations have proper dimensions */
        for (int rot = 0; rot < shape->n_rot; rot++) {
            assert_test(shape->rot_wh[rot].x > 0 && shape->rot_wh[rot].x <= 4,
                        "tetromino %d rotation %d width should be 1-4", i, rot);
            assert_test(shape->rot_wh[rot].y > 0 && shape->rot_wh[rot].y <= 4,
                        "tetromino %d rotation %d height should be 1-4", i,
                        rot);

            /* Validate block coordinates are within bounding box */
            for (int block = 0; block < MAX_BLOCK_LEN; block++) {
                int x = shape->rot_flat[rot][block][0];
                int y = shape->rot_flat[rot][block][1];

                if (x >= 0 && y >= 0) { /* Valid coordinates */
                    assert_test(
                        x < shape->rot_wh[rot].x && y < shape->rot_wh[rot].y,
                        "tetromino %d rotation %d block %d coordinates (%d,%d) "
                        "should be within bounds (%d,%d)",
                        i, rot, block, x, y, shape->rot_wh[rot].x,
                        shape->rot_wh[rot].y);
                }
            }
        }

        /* Validate collision detection data (crust) */
        for (int rot = 0; rot < shape->n_rot; rot++) {
            for (direction_t d = 0; d < 4; d++) {
                assert_test(shape->crust_len[rot][d] >= 0 &&
                                shape->crust_len[rot][d] <= MAX_BLOCK_LEN,
                            "tetromino %d rotation %d direction %d crust "
                            "length should be reasonable",
                            i, rot, d);
            }
        }
    }

    shapes_free();
}

void test_shape_rotation_consistency(void)
{
    bool init_result = shapes_init();
    assert_test(init_result, "shapes_init should succeed for rotation tests");

    if (!init_result)
        return;

    /* Test that rot and rot_flat contain consistent data */
    for (int i = 0; i < NUM_TETRIS_SHAPES; i++) {
        shape_t *shape = shape_get_by_index(i);
        if (!shape)
            continue;

        for (int rot = 0; rot < shape->n_rot && rot < 4; rot++) {
            /* Compare rot array with rot_flat array */
            for (int block = 0; block < MAX_BLOCK_LEN; block++) {
                if (!shape->rot[rot] || !shape->rot[rot][block])
                    continue;

                int rot_x = shape->rot[rot][block][0];
                int rot_y = shape->rot[rot][block][1];
                int flat_x = shape->rot_flat[rot][block][0];
                int flat_y = shape->rot_flat[rot][block][1];

                assert_test(rot_x == flat_x && rot_y == flat_y,
                            "tetromino %d rotation %d block %d: rot and "
                            "rot_flat should match (%d,%d) vs (%d,%d)",
                            i, rot, block, rot_x, rot_y, flat_x, flat_y);
            }
        }
    }

    shapes_free();
}

void test_shape_crust_data_validation(void)
{
    bool init_result = shapes_init();
    assert_test(init_result, "shapes_init should succeed for crust tests");

    if (!init_result)
        return;

    /* Test collision detection crust data consistency */
    for (int i = 0; i < NUM_TETRIS_SHAPES; i++) {
        shape_t *shape = shape_get_by_index(i);
        if (!shape)
            continue;

        for (int rot = 0; rot < shape->n_rot && rot < 4; rot++) {
            for (direction_t d = 0; d < 4; d++) {
                int crust_len = shape->crust_len[rot][d];

                /* Verify crust and crust_flat consistency */
                for (int c = 0; c < crust_len && c < MAX_BLOCK_LEN; c++) {
                    if (shape->crust[rot][d] && shape->crust[rot][d][c]) {
                        int crust_x = shape->crust[rot][d][c][0];
                        int crust_y = shape->crust[rot][d][c][1];
                        int flat_x = shape->crust_flat[rot][d][c][0];
                        int flat_y = shape->crust_flat[rot][d][c][1];

                        assert_test(
                            crust_x == flat_x && crust_y == flat_y,
                            "tetromino %d rotation %d direction %d crust %d: "
                            "crust and crust_flat should match (%d,%d) vs "
                            "(%d,%d)",
                            i, rot, d, c, crust_x, crust_y, flat_x, flat_y);

                        /* Verify crust coordinates are within shape bounds */
                        assert_test(crust_x >= 0 && crust_y >= 0 &&
                                        crust_x < shape->rot_wh[rot].x &&
                                        crust_y < shape->rot_wh[rot].y,
                                    "tetromino %d rotation %d direction %d "
                                    "crust %d coordinates (%d,%d) should be "
                                    "within bounds (%d,%d)",
                                    i, rot, d, c, crust_x, crust_y,
                                    shape->rot_wh[rot].x, shape->rot_wh[rot].y);
                    }
                }
            }
        }
    }

    shapes_free();
}

void test_shape_stream_basic_operations(void)
{
    bool init_result = shapes_init();
    assert_test(init_result, "shapes_init should succeed for stream tests");

    if (!init_result)
        return;

    /* Test stream creation and basic operations */
    shape_stream_t *stream = shape_stream_new();
    assert_test(stream, "shape_stream_new should return valid stream");

    if (!stream) {
        shapes_free();
        return;
    }

    /* Test peek operations for next piece preview */
    shape_t *next_piece = shape_stream_peek(stream, 0);
    assert_test(next_piece, "peek at next piece should return valid shape");

    shape_t *preview_piece = shape_stream_peek(stream, 1);
    assert_test(preview_piece,
                "peek at preview piece should return valid shape");

    /* Test that peek doesn't advance stream */
    shape_t *next_again = shape_stream_peek(stream, 0);
    assert_test(next_again == next_piece,
                "multiple peeks should return same piece");

    /* Test piece consumption (pop) */
    shape_t *current_piece = shape_stream_pop(stream);
    assert_test(current_piece == next_piece,
                "popped piece should match peeked piece");

    /* Test stream advancement */
    shape_t *new_next = shape_stream_peek(stream, 0);
    assert_test(new_next == preview_piece,
                "after pop, next piece should be old preview piece");

    nfree(stream);
    shapes_free();
}

void test_shape_stream_bounds_and_edge_cases(void)
{
    bool init_result = shapes_init();
    assert_test(init_result, "shapes_init should succeed for edge case tests");

    if (!init_result)
        return;

    shape_stream_t *stream = shape_stream_new();
    if (!stream) {
        shapes_free();
        return;
    }

    /* Test bounds checking for peek operations */
    assert_test(shape_stream_peek(stream, -2) == NULL,
                "negative peek index should return NULL");
    assert_test(shape_stream_peek(stream, stream->max_len) == NULL,
                "peek beyond max_len should return NULL");

    /* Test NULL stream handling */
    assert_test(shape_stream_peek(NULL, 0) == NULL,
                "peek with NULL stream should return NULL");
    assert_test(shape_stream_pop(NULL) == NULL,
                "pop with NULL stream should return NULL");

    /* Test sustained piece generation (multiple bags) */
    bool sustained_generation = true;
    for (int i = 0; i < 21; i++) { /* 3 complete 7-bags */
        shape_t *piece = shape_stream_pop(stream);
        if (!piece) {
            sustained_generation = false;
            break;
        }
    }
    assert_test(sustained_generation,
                "stream should provide pieces across multiple 7-bags");

    nfree(stream);
    shapes_free();
}

void test_shape_stream_7bag_randomization(void)
{
    bool init_result = shapes_init();
    assert_test(init_result, "shapes_init should succeed for 7-bag tests");

    if (!init_result)
        return;

    /* Reset bag to ensure fresh test state */
    reset_shape_bag();

    shape_stream_t *stream = shape_stream_new();
    if (!stream) {
        shapes_free();
        return;
    }

    /* Test the 7-bag algorithm: every 7 pieces should contain each tetromino
     * exactly once
     */
    const int bag_size = 7;
    shape_t *bag_pieces[bag_size];
    int piece_counts[NUM_TETRIS_SHAPES] = {0};

    /* Get first complete bag */
    bool valid_bag = true;
    for (int i = 0; i < bag_size; i++) {
        bag_pieces[i] = shape_stream_pop(stream);
        if (!bag_pieces[i]) {
            valid_bag = false;
            break;
        }

        /* Count occurrences of each tetromino type */
        for (int shape_idx = 0; shape_idx < NUM_TETRIS_SHAPES; shape_idx++) {
            if (bag_pieces[i] == shape_get_by_index(shape_idx)) {
                piece_counts[shape_idx]++;
                break;
            }
        }
    }

    assert_test(valid_bag, "should be able to get complete 7-piece bag");

    if (valid_bag) {
        /* Verify 7-bag property: each tetromino appears exactly once */
        int unique_pieces = 0;
        bool perfect_distribution = true;

        for (int i = 0; i < NUM_TETRIS_SHAPES; i++) {
            if (piece_counts[i] == 1) {
                unique_pieces++;
            } else if (piece_counts[i] != 1) {
                perfect_distribution = false;
            }
        }

        assert_test(unique_pieces == NUM_TETRIS_SHAPES,
                    "7-bag should contain all %d tetromino types (got %d)",
                    NUM_TETRIS_SHAPES, unique_pieces);
        assert_test(perfect_distribution,
                    "each tetromino should appear exactly once per bag");
    }

    nfree(stream);
    shapes_free();
}

void test_shape_stream_multiple_bags_distribution(void)
{
    bool init_result = shapes_init();
    assert_test(init_result,
                "shapes_init should succeed for multi-bag distribution tests");

    if (!init_result)
        return;

    /* Reset bag for consistent test state */
    reset_shape_bag();

    shape_stream_t *stream = shape_stream_new();
    if (!stream) {
        shapes_free();
        return;
    }

    /* Test distribution across multiple complete 7-bags */
    const int num_bags = 5;
    const int total_pieces = num_bags * 7;
    int total_counts[NUM_TETRIS_SHAPES] = {0};

    bool multi_bag_valid = true;
    for (int piece_num = 0; piece_num < total_pieces; piece_num++) {
        shape_t *piece = shape_stream_pop(stream);
        if (!piece) {
            multi_bag_valid = false;
            break;
        }

        /* Count each tetromino type across all bags */
        for (int shape_idx = 0; shape_idx < NUM_TETRIS_SHAPES; shape_idx++) {
            if (piece == shape_get_by_index(shape_idx)) {
                total_counts[shape_idx]++;
                break;
            }
        }
    }

    assert_test(multi_bag_valid,
                "should be able to get pieces from multiple bags");

    if (multi_bag_valid) {
        /* Verify perfect distribution: each piece should appear exactly
         * num_bags times */
        bool perfect_multi_bag_distribution = true;
        for (int i = 0; i < NUM_TETRIS_SHAPES; i++) {
            if (total_counts[i] != num_bags) {
                perfect_multi_bag_distribution = false;
                break;
            }
        }

        assert_test(perfect_multi_bag_distribution,
                    "across %d bags, each tetromino should appear exactly %d "
                    "times",
                    num_bags, num_bags);
    }

    nfree(stream);
    shapes_free();
}

void test_shape_stream_reset_functionality(void)
{
    bool init_result = shapes_init();
    assert_test(init_result, "shapes_init should succeed for reset tests");

    if (!init_result)
        return;

    shape_stream_t *stream = shape_stream_new();
    if (!stream) {
        shapes_free();
        return;
    }

    /* Get some pieces to advance the bag state */
    shape_t *piece1 = shape_stream_pop(stream);
    shape_t *piece2 = shape_stream_pop(stream);
    shape_t *piece3 = shape_stream_pop(stream);

    assert_test(piece1 && piece2 && piece3,
                "should be able to get initial pieces");

    /* Reset the bag and test that we can still get pieces */
    reset_shape_bag();

    /* Create new stream to test post-reset behavior */
    shape_stream_t *new_stream = shape_stream_new();
    if (!new_stream) {
        nfree(stream);
        shapes_free();
        return;
    }

    shape_t *reset_piece = shape_stream_pop(new_stream);
    assert_test(reset_piece, "should be able to get piece after reset");

    /* Test that reset doesn't affect existing streams immediately */
    shape_t *old_stream_piece = shape_stream_pop(stream);
    assert_test(old_stream_piece,
                "existing stream should continue working after reset");

    nfree(stream);
    nfree(new_stream);
    shapes_free();
}

void test_shape_stream_gameplay_sequence(void)
{
    bool init_result = shapes_init();
    assert_test(init_result,
                "shapes_init should succeed for gameplay sequence tests");

    if (!init_result)
        return;

    shape_stream_t *stream = shape_stream_new();
    if (!stream) {
        shapes_free();
        return;
    }

    /* Simulate realistic Tetris gameplay: get pieces, preview next pieces */
    const int game_pieces = 50; /* Simulate reasonable game length */
    bool gameplay_valid = true;
    int pieces_seen[NUM_TETRIS_SHAPES] = {0};

    for (int piece_num = 0; piece_num < game_pieces; piece_num++) {
        /* Get current piece (like starting a new piece drop) */
        shape_t *current = shape_stream_pop(stream);
        if (!current) {
            gameplay_valid = false;
            break;
        }

        /* Preview next piece (typical Tetris feature) */
        shape_t *preview = shape_stream_peek(stream, 0);
        if (!preview) {
            gameplay_valid = false;
            break;
        }

        /* Count piece distribution for fairness analysis */
        for (int shape_idx = 0; shape_idx < NUM_TETRIS_SHAPES; shape_idx++) {
            if (current == shape_get_by_index(shape_idx)) {
                pieces_seen[shape_idx]++;
                break;
            }
        }
    }

    assert_test(gameplay_valid,
                "gameplay sequence should provide valid pieces");

    if (gameplay_valid) {
        /* Verify fair distribution over longer gameplay */
        int min_seen = game_pieces;
        int max_seen = 0;

        for (int i = 0; i < NUM_TETRIS_SHAPES; i++) {
            if (pieces_seen[i] < min_seen)
                min_seen = pieces_seen[i];
            if (pieces_seen[i] > max_seen)
                max_seen = pieces_seen[i];
        }

        /* In fair 7-bag system, distribution should be relatively even */
        int distribution_range = max_seen - min_seen;
        assert_test(distribution_range <= NUM_TETRIS_SHAPES,
                    "piece distribution should be fair (range: %d)",
                    distribution_range);
    }

    nfree(stream);
    shapes_free();
}

void test_shape_stream_memory_management(void)
{
    bool init_result = shapes_init();
    assert_test(init_result,
                "shapes_init should succeed for memory management tests");

    if (!init_result)
        return;

    /* Test multiple stream creation and cleanup */
    const int num_streams = 10;
    shape_stream_t *streams[num_streams];

    /* Create multiple streams */
    bool creation_success = true;
    for (int i = 0; i < num_streams; i++) {
        streams[i] = shape_stream_new();
        if (!streams[i]) {
            creation_success = false;
            break;
        }
    }

    assert_test(creation_success, "should be able to create multiple streams");

    if (creation_success) {
        /* Test that all streams work independently */
        bool all_streams_work = true;
        for (int i = 0; i < num_streams; i++) {
            shape_t *piece = shape_stream_peek(streams[i], 0);
            if (!piece) {
                all_streams_work = false;
                break;
            }
        }

        assert_test(all_streams_work,
                    "all created streams should provide valid pieces");

        /* Clean up all streams */
        for (int i = 0; i < num_streams; i++) {
            if (streams[i])
                nfree(streams[i]);
        }
    }

    shapes_free();
}

void test_shape_multiple_init_cleanup_cycles(void)
{
    /* Test robustness of multiple init/cleanup cycles */
    for (int cycle = 0; cycle < 3; cycle++) {
        bool init_result = shapes_init();
        assert_test(init_result, "init cycle %d should succeed", cycle);

        if (init_result) {
            /* Quick validation in each cycle */
            shape_t *test_shape = shape_get_by_index(0);
            assert_test(test_shape, "shape access should work in cycle %d",
                        cycle);

            shape_stream_t *test_stream = shape_stream_new();
            if (test_stream) {
                shape_t *test_piece = shape_stream_peek(test_stream, 0);
                assert_test(test_piece, "stream should work in cycle %d",
                            cycle);
                nfree(test_stream);
            }

            shapes_free();
        }
    }
}

void test_shape_edge_cases(void)
{
    /* Test operations before initialization */
    assert_test(shape_get_by_index(0) == NULL,
                "shape access before init should return NULL");

    /* Test shape stream before initialization */
    shape_stream_t *uninit_stream = shape_stream_new();
    if (uninit_stream) {
        /* Stream creation might work, but should handle uninitialized shapes */
        shape_stream_peek(uninit_stream, 0); /* Should not crash */
        assert_test(true, "stream operations before init should not crash");
        nfree(uninit_stream);
    }

    /* Test double cleanup safety */
    shapes_free();
    shapes_free(); /* Should be safe */
    assert_test(true, "multiple shapes_free calls should be safe");

    /* Test operations after cleanup */
    assert_test(shape_get_by_index(0) == NULL,
                "shape access after cleanup should return NULL");

    /* Test reset_shape_bag without initialization */
    reset_shape_bag(); /* Should not crash */
    assert_test(true, "reset_shape_bag before init should not crash");
}
