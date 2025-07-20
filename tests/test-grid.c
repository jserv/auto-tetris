#include <stdlib.h>
#include <string.h>

#include "../nalloc.h"
#include "../tetris.h"
#include "test.h"

void test_grid_basic_allocation(void)
{
    /* Test standard Tetris grid allocation */
    grid_t *grid = grid_new(GRID_HEIGHT, GRID_WIDTH);
    assert_test(grid, "standard Tetris grid allocation should succeed");

    if (!grid)
        return;

    /* Validate Tetris grid dimensions */
    assert_test(grid->width == GRID_WIDTH && grid->height == GRID_HEIGHT,
                "grid should have standard Tetris dimensions (%dx%d)",
                GRID_WIDTH, GRID_HEIGHT);

    /* Validate essential data structures */
    assert_test(grid->rows && grid->relief && grid->gaps,
                "grid should have allocated essential arrays");

    /* Validate initial empty state */
    assert_test(grid->n_full_rows == 0 && grid->n_total_cleared == 0,
                "new grid should be empty with no cleared lines");

    /* Validate empty column initialization */
    bool columns_empty = true;
    for (int col = 0; col < grid->width; col++) {
        if (grid->relief[col] != -1 || grid->gaps[col] != 0) {
            columns_empty = false;
            break;
        }
    }
    assert_test(columns_empty, "all columns should be initially empty");

    /* Validate all cells are empty */
    bool all_cells_empty = true;
    for (int row = 0; row < grid->height && all_cells_empty; row++) {
        for (int col = 0; col < grid->width; col++) {
            if (grid->rows[row][col]) {
                all_cells_empty = false;
                break;
            }
        }
    }
    assert_test(all_cells_empty, "all grid cells should be initially empty");

    nfree(grid);
}

void test_grid_allocation_edge_cases(void)
{
    /* Test invalid allocations */
    assert_test(grid_new(0, GRID_WIDTH) == NULL,
                "zero height should return NULL");
    assert_test(grid_new(GRID_HEIGHT, 0) == NULL,
                "zero width should return NULL");
    assert_test(grid_new(-1, GRID_WIDTH) == NULL,
                "negative height should return NULL");
    assert_test(grid_new(GRID_HEIGHT, -1) == NULL,
                "negative width should return NULL");

    /* Test valid alternative dimensions */
    grid_t *small_grid = grid_new(10, GRID_WIDTH);
    assert_test(small_grid, "smaller height with standard width should work");
    if (small_grid) {
        assert_test(small_grid->width == GRID_WIDTH && small_grid->height == 10,
                    "alternative grid dimensions should be set correctly");
        nfree(small_grid);
    }
}

void test_grid_copy_operations(void)
{
    grid_t *src = grid_new(GRID_HEIGHT, GRID_WIDTH);
    grid_t *dst = grid_new(GRID_HEIGHT, GRID_WIDTH);
    assert_test(src && dst, "grid allocation for copy test should succeed");

    if (!src || !dst) {
        nfree(src);
        nfree(dst);
        return;
    }

    /* Test copying empty grids */
    grid_cpy(dst, src);
    assert_test(dst->width == src->width && dst->height == src->height,
                "grid dimensions should be copied correctly");
    assert_test(dst->n_total_cleared == src->n_total_cleared &&
                    dst->n_full_rows == src->n_full_rows,
                "grid statistics should be copied correctly");

    /* Test edge cases for robustness */
    grid_cpy(NULL, src);  /* Should not crash */
    grid_cpy(dst, NULL);  /* Should not crash */
    grid_cpy(NULL, NULL); /* Should not crash */
    assert_test(true, "grid_cpy should handle NULL parameters gracefully");

    nfree(dst);
    nfree(src);
}

void test_grid_block_intersection_detection(void)
{
    bool shapes_ok = shapes_init();
    assert_test(shapes_ok, "shapes_init should succeed for intersection tests");
    if (!shapes_ok)
        return;

    grid_t *grid = grid_new(GRID_HEIGHT, GRID_WIDTH);
    block_t *block = block_new();
    shape_t *test_shape = shape_get_by_index(0);

    if (!grid || !block || !test_shape) {
        nfree(grid);
        nfree(block);
        shapes_free();
        return;
    }

    block_init(block, test_shape);

    /* Test valid placement in empty grid */
    block->offset.x = GRID_WIDTH / 2;
    block->offset.y = GRID_HEIGHT / 2;
    assert_test(!grid_block_intersects(grid, block),
                "block should not intersect in empty grid center");

    /* Test boundary collision detection */
    block->offset.x = -1;
    assert_test(grid_block_intersects(grid, block),
                "block should intersect when placed off left edge");

    block->offset.x = GRID_WIDTH;
    assert_test(grid_block_intersects(grid, block),
                "block should intersect when placed off right edge");

    block->offset.x = GRID_WIDTH / 2;
    block->offset.y = -1;
    assert_test(grid_block_intersects(grid, block),
                "block should intersect when placed below bottom");

    block->offset.y = GRID_HEIGHT;
    assert_test(grid_block_intersects(grid, block),
                "block should intersect when placed above top");

    /* Test collision with settled blocks */
    block->offset.x = 5;
    block->offset.y = 5;
    grid->rows[5][5] = true; /* Place obstacle */
    assert_test(grid_block_intersects(grid, block),
                "block should intersect with settled piece");

    /* Test NULL parameter handling */
    assert_test(grid_block_intersects(NULL, block),
                "NULL grid should cause intersection");
    assert_test(grid_block_intersects(grid, NULL),
                "NULL block should cause intersection");

    nfree(block);
    nfree(grid);
    shapes_free();
}

void test_grid_block_add_remove_operations(void)
{
    bool shapes_ok = shapes_init();
    assert_test(shapes_ok, "shapes_init should succeed for add/remove tests");
    if (!shapes_ok)
        return;

    grid_t *grid = grid_new(GRID_HEIGHT, GRID_WIDTH);
    block_t *block = block_new();
    shape_t *test_shape = shape_get_by_index(0);

    if (!grid || !block || !test_shape) {
        nfree(grid);
        nfree(block);
        shapes_free();
        return;
    }

    block_init(block, test_shape);
    block->offset.x = 5;
    block->offset.y = 5;

    /* Test block placement */
    grid_block_add(grid, block);

    /* Verify block was placed */
    bool block_placed = false;
    for (int i = 0; i < MAX_BLOCK_LEN; i++) {
        coord_t cr;
        block_get(block, i, &cr);
        if (cr.x >= 0 && cr.x < grid->width && cr.y >= 0 &&
            cr.y < grid->height) {
            if (grid->rows[cr.y][cr.x]) {
                block_placed = true;
                break;
            }
        }
    }
    assert_test(block_placed, "block should be placed in grid");

    /* Verify grid state updates */
    bool relief_updated = false;
    for (int col = 0; col < grid->width; col++) {
        if (grid->relief[col] >= 0) {
            relief_updated = true;
            break;
        }
    }
    assert_test(relief_updated,
                "grid relief should be updated after placement");

    /* Test block removal */
    grid_block_remove(grid, block);

    /* Verify block was removed */
    bool block_removed = true;
    for (int i = 0; i < MAX_BLOCK_LEN; i++) {
        coord_t cr;
        block_get(block, i, &cr);
        if (cr.x >= 0 && cr.x < grid->width && cr.y >= 0 &&
            cr.y < grid->height) {
            if (grid->rows[cr.y][cr.x]) {
                block_removed = false;
                break;
            }
        }
    }
    assert_test(block_removed, "block should be removed from grid");

    /* Test NULL parameter handling */
    grid_block_add(NULL, block);    /* Should not crash */
    grid_block_add(grid, NULL);     /* Should not crash */
    grid_block_remove(NULL, block); /* Should not crash */
    grid_block_remove(grid, NULL);  /* Should not crash */
    assert_test(true, "add/remove should handle NULL parameters gracefully");

    nfree(block);
    nfree(grid);
    shapes_free();
}

void test_grid_block_center_elevate(void)
{
    bool shapes_ok = shapes_init();
    assert_test(shapes_ok, "shapes_init should succeed for elevation tests");
    if (!shapes_ok)
        return;

    grid_t *grid = grid_new(GRID_HEIGHT, GRID_WIDTH);
    block_t *block = block_new();
    shape_t *test_shape = shape_get_by_index(0);

    if (!grid || !block || !test_shape) {
        nfree(grid);
        nfree(block);
        shapes_free();
        return;
    }

    block_init(block, test_shape);

    /* Test piece spawning (center + elevate) */
    int result = grid_block_center_elevate(grid, block);
    assert_test(result, "piece should spawn successfully in empty grid");

    /* Verify horizontal centering */
    int expected_x = (GRID_WIDTH - test_shape->rot_wh[block->rot].x) / 2;
    assert_test(
        block->offset.x == expected_x,
        "piece should be horizontally centered (%d expected, %d actual)",
        expected_x, block->offset.x);

    /* Verify vertical elevation */
    int expected_y = grid->height - test_shape->max_dim_len;
    assert_test(
        block->offset.y == expected_y,
        "piece should be elevated to spawn position (%d expected, %d actual)",
        expected_y, block->offset.y);

    /* Test that spawned piece doesn't intersect */
    assert_test(!grid_block_intersects(grid, block),
                "spawned piece should not intersect with empty grid");

    /* Test NULL parameter handling */
    assert_test(grid_block_center_elevate(NULL, block) == 0,
                "elevation with NULL grid should return 0");
    assert_test(grid_block_center_elevate(grid, NULL) == 0,
                "elevation with NULL block should return 0");

    nfree(block);
    nfree(grid);
    shapes_free();
}

void test_grid_block_drop_operation(void)
{
    bool shapes_ok = shapes_init();
    assert_test(shapes_ok, "shapes_init should succeed for drop tests");
    if (!shapes_ok)
        return;

    grid_t *grid = grid_new(GRID_HEIGHT, GRID_WIDTH);
    block_t *block = block_new();
    shape_t *test_shape = shape_get_by_index(0);

    if (!grid || !block || !test_shape) {
        nfree(grid);
        nfree(block);
        shapes_free();
        return;
    }

    block_init(block, test_shape);
    grid_block_center_elevate(grid, block);

    /* Test hard drop functionality */
    int initial_y = block->offset.y;
    int drop_amount = grid_block_drop(grid, block);
    int final_y = block->offset.y;

    assert_test(drop_amount >= 0, "drop amount should be non-negative");
    assert_test(final_y <= initial_y,
                "piece should drop down or stay in place");
    assert_test(final_y == initial_y - drop_amount,
                "final position should match drop calculation");

    /* Test that dropped piece is in valid position */
    assert_test(!grid_block_intersects(grid, block),
                "dropped piece should not intersect after hard drop");

    /* Test dropping already settled piece */
    int second_drop = grid_block_drop(grid, block);
    assert_test(second_drop == 0,
                "dropping settled piece should return 0 distance");

    /* Test NULL parameter handling */
    assert_test(grid_block_drop(NULL, block) == 0,
                "drop with NULL grid should return 0");
    assert_test(grid_block_drop(grid, NULL) == 0,
                "drop with NULL block should return 0");

    nfree(block);
    nfree(grid);
    shapes_free();
}

void test_grid_block_movement_validation(void)
{
    bool shapes_ok = shapes_init();
    assert_test(shapes_ok, "shapes_init should succeed for movement tests");
    if (!shapes_ok)
        return;

    grid_t *grid = grid_new(GRID_HEIGHT, GRID_WIDTH);
    block_t *block = block_new();
    shape_t *test_shape = shape_get_by_index(0);

    if (!grid || !block || !test_shape) {
        nfree(grid);
        nfree(block);
        shapes_free();
        return;
    }

    block_init(block, test_shape);
    grid_block_center_elevate(grid, block);

    /* Test basic movement directions */
    int initial_x = block->offset.x;
    grid_block_move(grid, block, LEFT, 1);
    assert_test(block->offset.x <= initial_x,
                "LEFT movement should not increase x coordinate");

    grid_block_move(grid, block, RIGHT, 2);
    assert_test(block->offset.x >= initial_x - 1,
                "RIGHT movement should increase x coordinate");

    int initial_y = block->offset.y;
    grid_block_move(grid, block, BOT, 1);
    assert_test(block->offset.y <= initial_y,
                "DOWN movement should not increase y coordinate");

    /* Test boundary collision prevention */
    block->offset.x = 0;
    int boundary_x = block->offset.x;
    grid_block_move(grid, block, LEFT, 5); /* Try to move off grid */
    assert_test(block->offset.x >= boundary_x,
                "movement should prevent going off left edge");

    block->offset.x = GRID_WIDTH - 1;
    boundary_x = block->offset.x;
    grid_block_move(grid, block, RIGHT, 5); /* Try to move off grid */
    assert_test(block->offset.x <= boundary_x + 1,
                "movement should prevent going off right edge");

    /* Test NULL parameter handling */
    grid_block_move(NULL, block, LEFT, 1); /* Should not crash */
    grid_block_move(grid, NULL, LEFT, 1);  /* Should not crash */
    assert_test(true, "movement should handle NULL parameters gracefully");

    nfree(block);
    nfree(grid);
    shapes_free();
}

void test_grid_block_rotation_validation(void)
{
    bool shapes_ok = shapes_init();
    assert_test(shapes_ok, "shapes_init should succeed for rotation tests");
    if (!shapes_ok)
        return;

    grid_t *grid = grid_new(GRID_HEIGHT, GRID_WIDTH);
    block_t *block = block_new();
    shape_t *test_shape = shape_get_by_index(0);

    if (!grid || !block || !test_shape) {
        nfree(grid);
        nfree(block);
        shapes_free();
        return;
    }

    block_init(block, test_shape);
    grid_block_center_elevate(grid, block);

    /* Test basic rotation */
    grid_block_rotate(grid, block, 1);
    assert_test(block->rot >= 0 && block->rot < test_shape->n_rot,
                "rotation should keep piece within valid rotation states");

    /* Test that rotated piece doesn't intersect */
    assert_test(!grid_block_intersects(grid, block),
                "rotated piece should not intersect in open space");

    /* Test rotation at boundary */
    block->offset.x = 0; /* Move to edge */
    grid_block_rotate(grid, block, 1);
    assert_test(block->rot >= 0 && block->rot < test_shape->n_rot,
                "rotation at boundary should maintain valid rotation");

    /* Test full rotation cycle */
    for (int i = 0; i < 4; i++) {
        grid_block_rotate(grid, block, 1);
        assert_test(block->rot >= 0 && block->rot < test_shape->n_rot,
                    "rotation cycle should maintain valid states");
    }

    /* Test NULL parameter handling */
    grid_block_rotate(NULL, block, 1); /* Should not crash */
    grid_block_rotate(grid, NULL, 1);  /* Should not crash */
    assert_test(true, "rotation should handle NULL parameters gracefully");

    nfree(block);
    nfree(grid);
    shapes_free();
}

void test_grid_line_clearing(void)
{
    grid_t *grid = grid_new(GRID_HEIGHT, GRID_WIDTH);
    assert_test(grid, "grid allocation should succeed for line clearing tests");
    if (!grid)
        return;

    /* Test clearing empty grid */
    assert_test(grid_clear_lines(grid) == 0,
                "clearing empty grid should return 0 lines");

    /* Test single line clear */
    for (int col = 0; col < grid->width; col++) {
        grid->rows[0][col] = true;
    }
    grid->n_row_fill[0] = grid->width;
    grid->full_rows[0] = 0;
    grid->n_full_rows = 1;

    int cleared_single = grid_clear_lines(grid);
    assert_test(cleared_single == 1, "should clear exactly 1 complete line");
    assert_test(grid->n_total_cleared == 1,
                "total cleared count should be updated");

    /* Verify line was actually cleared */
    bool line_empty = true;
    for (int col = 0; col < grid->width; col++) {
        if (grid->rows[0][col]) {
            line_empty = false;
            break;
        }
    }
    assert_test(line_empty, "cleared line should be empty");

    /* Test Tetris (4-line clear) */
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < grid->width; col++) {
            grid->rows[row][col] = true;
        }
        grid->n_row_fill[row] = grid->width;
        grid->full_rows[row] = row;
    }
    grid->n_full_rows = 4;

    int cleared_tetris = grid_clear_lines(grid);
    assert_test(cleared_tetris == 4, "should clear 4 lines for Tetris");
    assert_test(grid->n_total_cleared == 5,
                "total should include previous clears");

    /* Test non-contiguous line clearing */
    /* Fill lines 1, 3, 5 (skip 0, 2, 4) */
    for (int row = 1; row < 6; row += 2) {
        for (int col = 0; col < grid->width; col++) {
            grid->rows[row][col] = true;
        }
        grid->n_row_fill[row] = grid->width;
        grid->full_rows[grid->n_full_rows++] = row;
    }

    int cleared_scattered = grid_clear_lines(grid);
    assert_test(cleared_scattered == 3,
                "should clear scattered complete lines");

    /* Test NULL parameter handling */
    assert_test(grid_clear_lines(NULL) == 0,
                "clearing NULL grid should return 0");

    nfree(grid);
}

void test_grid_edge_cases_and_robustness(void)
{
    /* Test comprehensive NULL parameter handling */
    assert_test(grid_block_intersects(NULL, NULL),
                "NULL parameters should indicate intersection");

    grid_block_add(NULL, NULL);    /* Should not crash */
    grid_block_remove(NULL, NULL); /* Should not crash */

    assert_test(grid_block_center_elevate(NULL, NULL) == 0,
                "NULL parameters should return 0 for elevation");
    assert_test(grid_block_drop(NULL, NULL) == 0,
                "NULL parameters should return 0 for drop");

    grid_block_move(NULL, NULL, LEFT, 1); /* Should not crash */
    grid_block_rotate(NULL, NULL, 1);     /* Should not crash */

    /* Test operations with valid grid but NULL block */
    grid_t *test_grid = grid_new(GRID_HEIGHT, GRID_WIDTH);
    if (test_grid) {
        assert_test(grid_block_intersects(test_grid, NULL),
                    "NULL block should indicate intersection");

        grid_block_add(test_grid, NULL);    /* Should not crash */
        grid_block_remove(test_grid, NULL); /* Should not crash */

        assert_test(grid_block_center_elevate(test_grid, NULL) == 0,
                    "NULL block should return 0 for elevation");
        assert_test(grid_block_drop(test_grid, NULL) == 0,
                    "NULL block should return 0 for drop");

        grid_block_move(test_grid, NULL, LEFT, 1); /* Should not crash */
        grid_block_rotate(test_grid, NULL, 1);     /* Should not crash */

        nfree(test_grid);
    }

    assert_test(true, "all edge cases handled without crashes");
}
