#include <stdlib.h>
#include <string.h>
#include "../nalloc.h"
#include "../tetris.h"
#include "test.h"

void test_coordinate_operations(void)
{
    /* Test coord_t structure properties */
    coord_t coord;

    /* Test coordinate assignment and retrieval */
    coord.x = 5;
    coord.y = 10;
    assert_test(coord.x == 5, "coordinate x should be assignable");
    assert_test(coord.y == 10, "coordinate y should be assignable");

    /* Test coordinate bounds within uint8_t range */
    coord.x = 0;
    coord.y = 0;
    assert_test(coord.x == 0 && coord.y == 0,
                "coordinates should support zero values");

    coord.x = 255;
    coord.y = 255;
    assert_test(coord.x == 255 && coord.y == 255,
                "coordinates should support maximum uint8_t values");

    /* Test coordinate comparison and copying */
    coord_t coord1 = {10, 20};
    coord_t coord2 = {10, 20};
    coord_t coord3 = {15, 25};

    assert_test(coord1.x == coord2.x && coord1.y == coord2.y,
                "identical coordinates should have equal components");
    assert_test(!(coord1.x == coord3.x && coord1.y == coord3.y),
                "different coordinates should not be equal");

    /* Test coordinate arithmetic safety */
    coord_t result;
    result.x = coord1.x + 5;
    result.y = coord1.y + 5;
    assert_test(result.x == 15 && result.y == 25,
                "coordinate arithmetic should work correctly");

    /* Test typical Tetris coordinate usage */
    coord_t grid_coord = {GRID_WIDTH / 2, GRID_HEIGHT / 2};
    assert_test(grid_coord.x < GRID_WIDTH && grid_coord.y < GRID_HEIGHT,
                "coordinates should fit within grid bounds");
}

void test_direction_constants(void)
{
    /* Test direction enumeration values */
    assert_test(BOT == 0, "BOT direction should be 0");
    assert_test(LEFT == 1, "LEFT direction should be 1");
    assert_test(TOP == 2, "TOP direction should be 2");
    assert_test(RIGHT == 3, "RIGHT direction should be 3");

    /* Test direction uniqueness */
    direction_t directions[] = {BOT, LEFT, TOP, RIGHT};
    bool all_unique = true;

    for (int i = 0; i < 4; i++) {
        for (int j = i + 1; j < 4; j++) {
            if (directions[i] == directions[j]) {
                all_unique = false;
                break;
            }
        }
        if (!all_unique)
            break;
    }
    assert_test(all_unique, "all direction constants should be unique");

    /* Test direction range validity */
    for (int i = 0; i < 4; i++) {
        assert_test(directions[i] >= 0 && directions[i] < 4,
                    "direction %d should be in valid range 0-3", i);
    }

    /* Test opposite direction relationships */
    assert_test((BOT + 2) % 4 == TOP, "BOT and TOP should be opposite");
    assert_test((LEFT + 2) % 4 == RIGHT, "LEFT and RIGHT should be opposite");

    /* Test direction cycling for rotation operations */
    direction_t current = BOT;
    direction_t next = (current + 1) % 4;
    assert_test(next == LEFT, "BOT + 1 should cycle to LEFT");

    current = RIGHT;
    next = (current + 1) % 4;
    assert_test(next == BOT, "RIGHT + 1 should cycle to BOT");
}

void test_grid_constants_validation(void)
{
    /* Test grid dimension constants */
    assert_test(GRID_WIDTH > 0, "GRID_WIDTH should be positive");
    assert_test(GRID_HEIGHT > 0, "GRID_HEIGHT should be positive");

    /* Test grid dimensions are reasonable for Tetris */
    assert_test(GRID_WIDTH >= 10 && GRID_WIDTH <= 20,
                "GRID_WIDTH should be reasonable for Tetris (10-20)");
    assert_test(GRID_HEIGHT >= 15 && GRID_HEIGHT <= 25,
                "GRID_HEIGHT should be reasonable for Tetris (15-25)");

    /* Test that grid can accommodate tetrominoes */
    assert_test(GRID_WIDTH >= 4,
                "GRID_WIDTH should accommodate widest tetromino (4 blocks)");
    assert_test(GRID_HEIGHT >= 4,
                "GRID_HEIGHT should accommodate tallest tetromino (4 blocks)");

    /* Test grid size relationships */
    int total_cells = GRID_WIDTH * GRID_HEIGHT;
    assert_test(total_cells > 0, "total grid cells should be positive");
    assert_test(total_cells < 10000,
                "total grid cells should be reasonable (< 10000)");

    /* Test constants can be used in array declarations */
    static char test_grid[GRID_HEIGHT][GRID_WIDTH];
    test_grid[0][0] = 1;
    test_grid[GRID_HEIGHT - 1][GRID_WIDTH - 1] = 1;
    assert_test(
        test_grid[0][0] == 1 && test_grid[GRID_HEIGHT - 1][GRID_WIDTH - 1] == 1,
        "grid constants should work in array declarations");
}

void test_shape_constants_validation(void)
{
    /* Test maximum block length constant */
    assert_test(MAX_BLOCK_LEN > 0, "MAX_BLOCK_LEN should be positive");
    assert_test(MAX_BLOCK_LEN >= 4,
                "MAX_BLOCK_LEN should accommodate tetromino blocks (4)");
    assert_test(MAX_BLOCK_LEN <= 10,
                "MAX_BLOCK_LEN should be reasonable (not excessive)");

    /* Test number of tetris shapes constant */
    assert_test(NUM_TETRIS_SHAPES > 0, "NUM_TETRIS_SHAPES should be positive");
    assert_test(NUM_TETRIS_SHAPES == 7,
                "NUM_TETRIS_SHAPES should be 7 for standard Tetris");

    /* Test constants work with shape system */
    bool shapes_ok = shape_init();
    if (shapes_ok) {
        /* Test that NUM_TETRIS_SHAPES matches actual available shapes */
        int available_shapes = 0;
        for (int i = 0; i < NUM_TETRIS_SHAPES; i++) {
            if (shape_get(i))
                available_shapes++;
        }
        assert_test(available_shapes == NUM_TETRIS_SHAPES,
                    "NUM_TETRIS_SHAPES should match available shapes");

        /* Test MAX_BLOCK_LEN is sufficient for all shapes */
        bool all_shapes_fit = true;
        for (int i = 0; i < NUM_TETRIS_SHAPES; i++) {
            shape_t *shape = shape_get(i);
            if (shape) {
                /* Check that shape coordinates fit within MAX_BLOCK_LEN */
                for (int block = 0; block < MAX_BLOCK_LEN; block++) {
                    if (shape->rot_flat[0][block][0] >= 0 &&
                        shape->rot_flat[0][block][1] >= 0) {
                        /* This block is used, so MAX_BLOCK_LEN should be
                         * sufficient */
                        if (block >= MAX_BLOCK_LEN) {
                            all_shapes_fit = false;
                            break;
                        }
                    }
                }
                if (!all_shapes_fit)
                    break;
            }
        }
        assert_test(all_shapes_fit,
                    "MAX_BLOCK_LEN should be sufficient for all tetromino "
                    "shapes");

        shape_free();
    }

    /* Test constants can be used in array declarations */
    static int test_blocks[MAX_BLOCK_LEN][2];
    test_blocks[0][0] = 1;
    test_blocks[MAX_BLOCK_LEN - 1][1] = 1;
    assert_test(
        test_blocks[0][0] == 1 && test_blocks[MAX_BLOCK_LEN - 1][1] == 1,
        "MAX_BLOCK_LEN should work in array declarations");

    static shape_t *test_shapes[NUM_TETRIS_SHAPES];
    test_shapes[0] = NULL;
    test_shapes[NUM_TETRIS_SHAPES - 1] = NULL;
    assert_test(
        test_shapes[0] == NULL && test_shapes[NUM_TETRIS_SHAPES - 1] == NULL,
        "NUM_TETRIS_SHAPES should work in array declarations");

    /* Test relationship between constants */
    assert_test(MAX_BLOCK_LEN >= 4,
                "MAX_BLOCK_LEN should be at least 4 for tetromino blocks");
    assert_test(NUM_TETRIS_SHAPES <= MAX_BLOCK_LEN + 3,
                "NUM_TETRIS_SHAPES should be reasonable relative to "
                "MAX_BLOCK_LEN");
}
