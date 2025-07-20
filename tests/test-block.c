#include <stdlib.h>
#include <string.h>
#include "../nalloc.h"
#include "../tetris.h"
#include "test.h"

void test_block_basic_allocation(void)
{
    /* Test basic block allocation */
    block_t *block = block_new();
    assert_test(block, "block_new should return non-NULL pointer");

    if (!block)
        return; /* Skip if allocation failed */

    /* Test initial state */
    assert_test(block->rot == 0, "new block should have rotation 0");
    assert_test(block->offset.x == 0, "new block should have offset.x = 0");
    assert_test(block->offset.y == 0, "new block should have offset.y = 0");
    assert_test(block->shape == NULL, "new block should have NULL shape");

    /* Test deallocation */
    nfree(block);
    assert_test(true, "block deallocation completed");
}

void test_block_initialization_with_shapes(void)
{
    /* Initialize shapes first */
    bool shapes_ok = shapes_init();
    assert_test(shapes_ok, "shapes_init should succeed");

    if (!shapes_ok)
        return; /* Skip if shapes initialization failed */

    /* Test block initialization with various shapes */
    for (int shape_idx = 0; shape_idx < NUM_TETRIS_SHAPES; shape_idx++) {
        shape_t *test_shape = shape_get_by_index(shape_idx);
        if (!test_shape) {
            assert_test(false,
                        "shape_get_by_index(%d) should return valid shape",
                        shape_idx);
            continue;
        }

        block_t *block = block_new();
        assert_test(block, "block allocation for shape %d should succeed",
                    shape_idx);

        if (!block)
            continue;

        /* Test initialization */
        block_init(block, test_shape);
        assert_test(block->shape == test_shape,
                    "block_init should set shape correctly");
        assert_test(block->rot == 0, "block_init should reset rotation to 0");
        assert_test(block->offset.x == 0,
                    "block_init should reset offset.x to 0");
        assert_test(block->offset.y == 0,
                    "block_init should reset offset.y to 0");

        nfree(block);
    }

    /* Test initialization with NULL shape */
    block_t *block = block_new();
    if (block) {
        block_init(block, NULL);
        assert_test(block->shape == NULL,
                    "block_init with NULL shape should be safe");
        nfree(block);
    }

    /* Cleanup shapes */
    shapes_free();
}

void test_block_coordinate_retrieval(void)
{
    /* Initialize shapes */
    bool shapes_ok = shapes_init();
    assert_test(shapes_ok, "shapes_init should succeed for coordinate tests");

    if (!shapes_ok)
        return;

    /* Test coordinate retrieval with first shape */
    shape_t *test_shape = shape_get_by_index(0);
    if (!test_shape) {
        shapes_free();
        return;
    }

    block_t *block = block_new();
    if (!block) {
        shapes_free();
        return;
    }

    block_init(block, test_shape);

    /* Test basic coordinate retrieval */
    coord_t result;
    for (int i = 0; i < MAX_BLOCK_LEN; i++) {
        block_get(block, i, &result);
        assert_test(
            result.x < 100 || result.x == 255,
            "block_get coordinate x should be valid or 255 for invalid");
        assert_test(
            result.y < 100 || result.y == 255,
            "block_get coordinate y should be valid or 255 for invalid");
    }

    /* Test with offset */
    block->offset.x = 5;
    block->offset.y = 10;

    coord_t offset_result;
    block_get(block, 0, &offset_result);

    /* Reset offset and get original coordinate */
    block->offset.x = 0;
    block->offset.y = 0;
    coord_t original_result;
    block_get(block, 0, &original_result);

    if (original_result.x != 255 && original_result.y != 255) {
        assert_test(offset_result.x == original_result.x + 5,
                    "block_get should apply x offset correctly");
        assert_test(offset_result.y == original_result.y + 10,
                    "block_get should apply y offset correctly");
    }

    /* Test bounds checking */
    block_get(block, -1, &result);
    assert_test(result.x == 255 && result.y == 255,
                "block_get with negative index should return (255, 255)");

    block_get(block, MAX_BLOCK_LEN, &result);
    assert_test(
        result.x == 255 && result.y == 255,
        "block_get with index >= MAX_BLOCK_LEN should return (255, 255)");

    /* Test with NULL result pointer */
    block_get(block, 0, NULL);
    assert_test(true, "block_get with NULL result should not crash");

    /* Test with NULL block */
    block_get(NULL, 0, &result);
    assert_test(result.x == 255 && result.y == 255,
                "block_get with NULL block should return (255, 255)");

    nfree(block);
    shapes_free();
}

void test_block_rotation_operations(void)
{
    /* Initialize shapes */
    bool shapes_ok = shapes_init();
    assert_test(shapes_ok, "shapes_init should succeed for rotation tests");

    if (!shapes_ok)
        return;

    shape_t *test_shape = shape_get_by_index(0);
    if (!test_shape) {
        shapes_free();
        return;
    }

    block_t *block = block_new();
    if (!block) {
        shapes_free();
        return;
    }

    block_init(block, test_shape);

    /* Test basic rotation */
    int initial_rot = block->rot;
    block_rotate(block, 1);
    assert_test(block->rot == (initial_rot + 1) % test_shape->n_rot,
                "block_rotate(1) should increment rotation correctly");

    /* Test multiple rotations */
    block_rotate(block, 2);
    assert_test(block->rot == (initial_rot + 3) % test_shape->n_rot,
                "block_rotate(2) should work correctly");

    /* Test negative rotation */
    block_rotate(block, -1);
    assert_test(block->rot == (initial_rot + 2) % test_shape->n_rot,
                "block_rotate(-1) should decrement rotation correctly");

    /* Test wraparound */
    block->rot = 0;
    block_rotate(block, -1);
    assert_test(block->rot == test_shape->n_rot - 1,
                "negative rotation should wrap around correctly");

    /* Test full rotation cycle */
    block->rot = 0;
    int n_rot = test_shape->n_rot;
    block_rotate(block, n_rot);
    assert_test(block->rot == 0,
                "rotating by n_rot should return to original rotation");

    /* NOTE: block_rotate(NULL, 1) would cause segfault - implementation doesn't
     * handle NULL
     */

    nfree(block);
    shapes_free();
}

void test_block_movement_operations(void)
{
    /* Initialize shapes */
    bool shapes_ok = shapes_init();
    assert_test(shapes_ok, "shapes_init should succeed for movement tests");

    if (!shapes_ok)
        return;

    shape_t *test_shape = shape_get_by_index(0);
    if (!test_shape) {
        shapes_free();
        return;
    }

    block_t *block = block_new();
    if (!block) {
        shapes_free();
        return;
    }

    block_init(block, test_shape);

    /* Test LEFT movement */
    block->offset.x = 10;
    block->offset.y = 10;
    block_move(block, LEFT, 3);
    assert_test(block->offset.x == 7,
                "LEFT movement should decrease x coordinate");
    assert_test(block->offset.y == 10,
                "LEFT movement should not affect y coordinate");

    /* Test RIGHT movement */
    block_move(block, RIGHT, 5);
    assert_test(block->offset.x == 12,
                "RIGHT movement should increase x coordinate");
    assert_test(block->offset.y == 10,
                "RIGHT movement should not affect y coordinate");

    /* Test BOT movement */
    block_move(block, BOT, 2);
    assert_test(block->offset.x == 12,
                "BOT movement should not affect x coordinate");
    assert_test(block->offset.y == 8,
                "BOT movement should decrease y coordinate");

    /* Test TOP movement */
    block_move(block, TOP, 4);
    assert_test(block->offset.x == 12,
                "TOP movement should not affect x coordinate");
    assert_test(block->offset.y == 12,
                "TOP movement should increase y coordinate");

    /* Test zero movement */
    int saved_x = block->offset.x;
    int saved_y = block->offset.y;
    block_move(block, LEFT, 0);
    assert_test(block->offset.x == saved_x && block->offset.y == saved_y,
                "zero movement should not change position");

    /* Test negative movement */
    block_move(block, LEFT, -3);
    assert_test(block->offset.x == saved_x + 3,
                "negative LEFT movement should move RIGHT");

    /* NOTE: block_move(NULL, LEFT, 1) would cause segfault - implementation
     * doesn't handle NULL
     */

    nfree(block);
    shapes_free();
}

void test_block_extreme_calculations(void)
{
    /* Initialize shapes */
    bool shapes_ok = shapes_init();
    assert_test(shapes_ok, "shapes_init should succeed for extreme tests");

    if (!shapes_ok)
        return;

    shape_t *test_shape = shape_get_by_index(0);
    if (!test_shape) {
        shapes_free();
        return;
    }

    block_t *block = block_new();
    if (!block) {
        shapes_free();
        return;
    }

    block_init(block, test_shape);

    /* Test basic extreme calculations */
    block->offset.x = 5;
    block->offset.y = 10;

    int left_extreme = block_extreme(block, LEFT);
    int right_extreme = block_extreme(block, RIGHT);
    int bot_extreme = block_extreme(block, BOT);
    int top_extreme = block_extreme(block, TOP);

    assert_test(left_extreme == block->offset.x,
                "LEFT extreme should equal x offset");
    assert_test(bot_extreme == block->offset.y,
                "BOT extreme should equal y offset");

    /* RIGHT and TOP extremes depend on shape dimensions */
    if (test_shape->rot_wh[block->rot].x > 0) {
        assert_test(right_extreme ==
                        block->offset.x + test_shape->rot_wh[block->rot].x - 1,
                    "RIGHT extreme should be offset + width - 1");
    }

    if (test_shape->rot_wh[block->rot].y > 0) {
        assert_test(top_extreme ==
                        block->offset.y + test_shape->rot_wh[block->rot].y - 1,
                    "TOP extreme should be offset + height - 1");
    }

    /* Test with different rotation */
    block_rotate(block, 1);
    int new_right_extreme = block_extreme(block, RIGHT);
    int new_top_extreme = block_extreme(block, TOP);

    /* Extremes should update with rotation if shape dimensions change */
    if (test_shape->rot_wh[0].x != test_shape->rot_wh[1].x) {
        assert_test(
            new_right_extreme != right_extreme,
            "RIGHT extreme should change with rotation if width changes");
    }

    if (test_shape->rot_wh[0].y != test_shape->rot_wh[1].y) {
        assert_test(
            new_top_extreme != top_extreme,
            "TOP extreme should change with rotation if height changes");
    }

    /* NOTE: block_extreme(NULL, LEFT) would cause segfault - implementation
     * doesn't handle NULL
     */

    nfree(block);
    shapes_free();
}

void test_block_edge_cases(void)
{
    /* Test operations on uninitialized block */
    block_t *block = block_new();
    if (block) {
        coord_t result;

        /* These should handle NULL shape gracefully */
        block_get(block, 0, &result);
        assert_test(
            result.x == 255 && result.y == 255,
            "block_get on uninitialized block should return (255, 255)");

        /* NOTE: block_rotate and block_move on uninitialized block would
         * segfault */
        /* block_rotate(block, 1); - would crash due to NULL shape->n_rot access
         */
        /* block_move(block, LEFT, 1); - would crash due to NULL shape access */

        /* block_extreme should work for LEFT/BOT (only accesses offset), but
         * not RIGHT/TOP (accesses shape)
         */
        int extreme = block_extreme(block, LEFT);
        assert_test(
            extreme == 0,
            "block_extreme(LEFT) on uninitialized block should return 0");

        nfree(block);
    }

    /* NOTE: Testing with corrupted/invalid pointers would cause segfault
     * block_get() only checks for NULL, not invalid pointers like 0xDEADBEEF
     */

    assert_test(true, "edge case tests completed");
}
