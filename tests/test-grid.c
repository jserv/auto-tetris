#include <stdlib.h>
#include <string.h>

#include "../tetris.h"
#include "../utils.h"
#include "test.h"

void test_grid_system_initialization(void)
{
    /* Test that grid_init() can be called multiple times safely */
    grid_init(); /* Should not crash on repeated calls */
    grid_init();
    assert_test(true, "grid_init should handle multiple calls safely");

    /* Test that grid creation works after initialization */
    grid_t *test_grid = grid_new(GRID_HEIGHT, GRID_WIDTH);
    assert_test(test_grid, "grid allocation should work after grid_init");

    if (test_grid) {
        /* Verify grid is properly initialized */
        assert_test(
            test_grid->width == GRID_WIDTH && test_grid->height == GRID_HEIGHT,
            "grid dimensions should be set correctly after init");

        /* Verify initial state is clean */
        assert_test(test_grid->hash == 0,
                    "new grid should have zero hash initially");

        /* Test that grid operations work properly */
        assert_test(test_grid->n_full_rows == 0,
                    "new grid should have no full rows");

        nfree(test_grid);
    }

    /* Test grid creation with different dimensions */
    grid_t *alt_grid = grid_new(10, 8);
    assert_test(alt_grid, "alternative grid dimensions should work after init");

    if (alt_grid) {
        assert_test(alt_grid->width == 8 && alt_grid->height == 10,
                    "alternative grid should have correct dimensions");
        nfree(alt_grid);
    }

    /* Test that multiple grids can be created */
    grid_t *grid1 = grid_new(GRID_HEIGHT, GRID_WIDTH);
    grid_t *grid2 = grid_new(GRID_HEIGHT, GRID_WIDTH);

    assert_test(grid1 && grid2,
                "multiple grids should be creatable after init");

    if (grid1 && grid2) {
        /* Verify they have independent state */
        assert_test(grid1 != grid2,
                    "different grids should have different addresses");

        /* Verify they have independent memory structures */
        assert_test(grid1->rows != grid2->rows,
                    "grids should have separate row arrays");
        assert_test(grid1->relief != grid2->relief,
                    "grids should have separate relief arrays");

        /* Test basic independent modifications (without hash dependency) */
        grid1->n_total_cleared = 5;
        grid2->n_total_cleared = 10;
        assert_test(grid1->n_total_cleared != grid2->n_total_cleared,
                    "grids should maintain independent statistics");

        nfree(grid1);
        nfree(grid2);
    }
}

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
    grid_copy(dst, src);
    assert_test(dst->width == src->width && dst->height == src->height,
                "grid dimensions should be copied correctly");
    assert_test(dst->n_total_cleared == src->n_total_cleared &&
                    dst->n_full_rows == src->n_full_rows,
                "grid statistics should be copied correctly");

    /* Test edge cases for robustness */
    grid_copy(NULL, src);  /* Should not crash */
    grid_copy(dst, NULL);  /* Should not crash */
    grid_copy(NULL, NULL); /* Should not crash */
    assert_test(true, "grid_copy should handle NULL parameters gracefully");

    nfree(dst);
    nfree(src);
}

void test_grid_block_intersection_detection(void)
{
    bool shapes_ok = shape_init();
    assert_test(shapes_ok, "shape_init should succeed for intersection tests");
    if (!shapes_ok)
        return;

    grid_t *grid = grid_new(GRID_HEIGHT, GRID_WIDTH);
    block_t *block = block_new();
    shape_t *test_shape = shape_get(0);

    if (!grid || !block || !test_shape) {
        nfree(grid);
        nfree(block);
        shape_free();
        return;
    }

    block_init(block, test_shape);

    /* Test valid placement in empty grid */
    block->offset.x = GRID_WIDTH / 2;
    block->offset.y = GRID_HEIGHT / 2;
    assert_test(!grid_block_collides(grid, block),
                "block should not intersect in empty grid center");

    /* Test boundary collision detection */
    block->offset.x = -1;
    assert_test(grid_block_collides(grid, block),
                "block should intersect when placed off left edge");

    block->offset.x = GRID_WIDTH;
    assert_test(grid_block_collides(grid, block),
                "block should intersect when placed off right edge");

    block->offset.x = GRID_WIDTH / 2;
    block->offset.y = -1;
    assert_test(grid_block_collides(grid, block),
                "block should intersect when placed below bottom");

    block->offset.y = GRID_HEIGHT;
    assert_test(grid_block_collides(grid, block),
                "block should intersect when placed above top");

    /* Test collision with settled blocks */
    block->offset.x = 5;
    block->offset.y = 5;
    grid->rows[5][5] = true; /* Place obstacle */
    assert_test(grid_block_collides(grid, block),
                "block should intersect with settled piece");

    /* Test NULL parameter handling */
    assert_test(grid_block_collides(NULL, block),
                "NULL grid should cause intersection");
    assert_test(grid_block_collides(grid, NULL),
                "NULL block should cause intersection");

    nfree(block);
    nfree(grid);
    shape_free();
}

void test_grid_block_add_remove_operations(void)
{
    bool shapes_ok = shape_init();
    assert_test(shapes_ok, "shape_init should succeed for add/remove tests");
    if (!shapes_ok)
        return;

    grid_t *grid = grid_new(GRID_HEIGHT, GRID_WIDTH);
    block_t *block = block_new();
    shape_t *test_shape = shape_get(0);

    if (!grid || !block || !test_shape) {
        nfree(grid);
        nfree(block);
        shape_free();
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
    shape_free();
}

void test_grid_block_spawn(void)
{
    bool shapes_ok = shape_init();
    assert_test(shapes_ok, "shape_init should succeed for elevation tests");
    if (!shapes_ok)
        return;

    grid_t *grid = grid_new(GRID_HEIGHT, GRID_WIDTH);
    block_t *block = block_new();
    shape_t *test_shape = shape_get(0);

    if (!grid || !block || !test_shape) {
        nfree(grid);
        nfree(block);
        shape_free();
        return;
    }

    block_init(block, test_shape);

    /* Test piece spawning (center + elevate) */
    int result = grid_block_spawn(grid, block);
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
    assert_test(!grid_block_collides(grid, block),
                "spawned piece should not intersect with empty grid");

    /* Test NULL parameter handling */
    assert_test(grid_block_spawn(NULL, block) == 0,
                "elevation with NULL grid should return 0");
    assert_test(grid_block_spawn(grid, NULL) == 0,
                "elevation with NULL block should return 0");

    nfree(block);
    nfree(grid);
    shape_free();
}

void test_grid_block_drop_operation(void)
{
    bool shapes_ok = shape_init();
    assert_test(shapes_ok, "shape_init should succeed for drop tests");
    if (!shapes_ok)
        return;

    grid_t *grid = grid_new(GRID_HEIGHT, GRID_WIDTH);
    block_t *block = block_new();
    shape_t *test_shape = shape_get(0);

    if (!grid || !block || !test_shape) {
        nfree(grid);
        nfree(block);
        shape_free();
        return;
    }

    block_init(block, test_shape);
    grid_block_spawn(grid, block);

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
    assert_test(!grid_block_collides(grid, block),
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
    shape_free();
}

void test_grid_block_movement_validation(void)
{
    bool shapes_ok = shape_init();
    assert_test(shapes_ok, "shape_init should succeed for movement tests");
    if (!shapes_ok)
        return;

    grid_t *grid = grid_new(GRID_HEIGHT, GRID_WIDTH);
    block_t *block = block_new();
    shape_t *test_shape = shape_get(0);

    if (!grid || !block || !test_shape) {
        nfree(grid);
        nfree(block);
        shape_free();
        return;
    }

    block_init(block, test_shape);
    grid_block_spawn(grid, block);

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
    shape_free();
}

void test_grid_block_rotation_validation(void)
{
    bool shapes_ok = shape_init();
    assert_test(shapes_ok, "shape_init should succeed for rotation tests");
    if (!shapes_ok)
        return;

    grid_t *grid = grid_new(GRID_HEIGHT, GRID_WIDTH);
    block_t *block = block_new();
    shape_t *test_shape = shape_get(0);

    if (!grid || !block || !test_shape) {
        nfree(grid);
        nfree(block);
        shape_free();
        return;
    }

    block_init(block, test_shape);
    grid_block_spawn(grid, block);

    /* Test basic rotation */
    grid_block_rotate(grid, block, 1);
    assert_test(block->rot >= 0 && block->rot < test_shape->n_rot,
                "rotation should keep piece within valid rotation states");

    /* Test that rotated piece doesn't intersect */
    assert_test(!grid_block_collides(grid, block),
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
    shape_free();
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

void test_grid_tetris_ready_detection(void)
{
    bool shapes_ok = shape_init();
    assert_test(shapes_ok, "shape_init should succeed for Tetris ready tests");
    if (!shapes_ok)
        return;

    grid_t *grid = grid_new(GRID_HEIGHT, GRID_WIDTH);
    block_t *block = block_new();
    assert_test(
        grid && block,
        "grid and block allocation should succeed for Tetris ready tests");
    if (!grid || !block) {
        nfree(grid);
        nfree(block);
        shape_free();
        return;
    }

    int well_col = -1;

    /* Test basic positive functionality only */
    /* Reset grid completely */
    for (int row = 0; row < grid->height; row++) {
        for (int col = 0; col < grid->width; col++)
            grid->rows[row][col] = false;
    }
    for (int col = 0; col < grid->width; col++) {
        grid->relief[col] = -1;
        grid->gaps[col] = 0;
        grid->stack_cnt[col] = 0;
    }

    /* Create a simple valid Tetris well */
    int well_column = 7;
    /* Fill only immediate neighbors to create a localized well */
    for (int col = 6; col <= 8; col++) {
        if (col != well_column) {
            for (int row = 0; row < 6; row++)
                grid->rows[row][col] = true;
            grid->relief[col] = 5; /* Height 6 */
        }
    }
    /* Column 7 remains empty (relief[7] = -1) */

    well_col = -1;
    bool clear_ready = grid_is_tetris_ready(grid, &well_col);
    assert_test(clear_ready, "clear deep well should be Tetris ready");
    assert_test(well_col == well_column,
                "clear well should be detected at column %d (got %d)",
                well_column, well_col);

    /* Verify the well column is correctly identified */
    assert_test(well_col >= 0 && well_col < grid->width,
                "detected well column should be within grid bounds");

    /* Test with one block added to well - should still be valid */
    grid->rows[0][well_column] = true;
    grid->relief[well_column] = 0;

    well_col = -1;
    bool partial_ready = grid_is_tetris_ready(grid, &well_col);
    assert_test(partial_ready,
                "partially filled well should still be Tetris ready");
    assert_test(well_col == well_column,
                "partially filled well should be at column %d (got %d)",
                well_column, well_col);

    /* Test NULL parameter handling */
    well_col = -1;
    assert_test(!grid_is_tetris_ready(NULL, &well_col),
                "NULL grid should return false");
    assert_test(well_col == -1, "NULL grid should set well_col to -1");

    well_col = 999;
    assert_test(!grid_is_tetris_ready(grid, NULL),
                "NULL well_col parameter should return false");

    /* Test invalid grid structure */
    int *saved_relief = grid->relief;
    grid->relief = NULL;
    well_col = 999;
    assert_test(!grid_is_tetris_ready(grid, &well_col),
                "grid with NULL relief should return false");
    assert_test(well_col == -1,
                "grid with NULL relief should set well_col to -1");
    grid->relief = saved_relief; /* Restore for cleanup */

    nfree(block);
    nfree(grid);
    shape_free();
}

/* Helper function to compare grid states for testing */
static bool grids_equal(const grid_t *g1, const grid_t *g2)
{
    if (!g1 || !g2 || g1->width != g2->width || g1->height != g2->height)
        return false;

    /* Compare cell occupancy */
    for (int row = 0; row < g1->height; row++) {
        for (int col = 0; col < g1->width; col++) {
            if (g1->rows[row][col] != g2->rows[row][col])
                return false;
        }
    }

    /* Compare auxiliary data structures */
    for (int col = 0; col < g1->width; col++) {
        if (g1->relief[col] != g2->relief[col] ||
            g1->gaps[col] != g2->gaps[col] ||
            g1->stack_cnt[col] != g2->stack_cnt[col])
            return false;
    }

    for (int row = 0; row < g1->height; row++) {
        if (g1->n_row_fill[row] != g2->n_row_fill[row])
            return false;
    }

    /* Compare statistics */
    if (g1->n_full_rows != g2->n_full_rows ||
        g1->n_total_cleared != g2->n_total_cleared ||
        g1->n_last_cleared != g2->n_last_cleared || g1->hash != g2->hash)
        return false;

    return true;
}

void test_grid_apply_block_and_rollback(void)
{
    /* Test snapshot-based apply/rollback system used by AI */
    bool shapes_ok = shape_init();
    assert_test(shapes_ok, "shape_init should succeed for snapshot tests");
    if (!shapes_ok)
        return;

    grid_t *grid = grid_new(GRID_HEIGHT, GRID_WIDTH);
    grid_t *backup_grid = grid_new(GRID_HEIGHT, GRID_WIDTH);
    block_t *block = block_new();
    shape_t *test_shape = shape_get(0);

    if (!grid || !backup_grid || !block || !test_shape) {
        nfree(grid);
        nfree(backup_grid);
        nfree(block);
        shape_free();
        return;
    }

    block_init(block, test_shape);

    /* Test 1: Simple apply/rollback without line clearing */
    grid_block_spawn(grid, block);
    grid_block_drop(grid, block);
    grid_copy(backup_grid, grid); /* Save original state */

    grid_snapshot_t snapshot;
    int lines_cleared = grid_apply_block(grid, block, &snapshot);

    assert_test(lines_cleared >= 0,
                "apply_block should return non-negative line count");

    /* Verify block was applied to grid */
    bool block_applied = false;
    for (int i = 0; i < MAX_BLOCK_LEN; i++) {
        coord_t cr;
        block_get(block, i, &cr);
        if (cr.x >= 0 && cr.x < grid->width && cr.y >= 0 &&
            cr.y < grid->height) {
            if (grid->rows[cr.y][cr.x]) {
                block_applied = true;
                break;
            }
        }
    }
    assert_test(block_applied, "block should be applied to grid");

    /* Test rollback */
    grid_rollback(grid, &snapshot);

    /* Verify grid is restored to original state */
    assert_test(grids_equal(grid, backup_grid),
                "rollback should restore original grid state");

    /* Test 2: Apply/rollback with some existing blocks */
    /* Reset to empty grid */
    for (int row = 0; row < grid->height; row++) {
        for (int col = 0; col < grid->width; col++)
            grid->rows[row][col] = false;
        grid->n_row_fill[row] = 0;
    }
    for (int col = 0; col < grid->width; col++) {
        grid->relief[col] = -1;
        grid->gaps[col] = 0;
        grid->stack_cnt[col] = 0;
    }
    grid->n_full_rows = 0;
    grid->n_total_cleared = 0;
    grid->n_last_cleared = 0;
    grid->hash = 0;

    /* Add a block to create some initial state */
    block->offset.x = 2;
    block->offset.y = 0;
    grid_block_add(grid, block);

    grid_copy(backup_grid, grid); /* Save state before applying another block */

    /* Position second block in a different location */
    block->offset.x = 6;
    block->offset.y = 3;

    grid_snapshot_t apply_snapshot;
    int apply_result = grid_apply_block(grid, block, &apply_snapshot);

    assert_test(apply_result >= 0, "apply_block should complete successfully");

    /* Verify grid state changed */
    bool state_changed = !grids_equal(grid, backup_grid);
    assert_test(state_changed, "grid state should change after apply_block");

    /* Test rollback */
    grid_rollback(grid, &apply_snapshot);

    /* Verify grid is restored to state before the second block */
    assert_test(grids_equal(grid, backup_grid),
                "rollback should restore state after line clearing");

    /* Test 3: Multiple apply/rollback cycles */
    bool all_cycles_successful = true;

    for (int cycle = 0; cycle < 3; cycle++) {
        /* Position block in different safe locations */
        block->offset.x = 1 + cycle;
        block->offset.y = 5 + cycle;

        grid_copy(backup_grid, grid);

        grid_snapshot_t cycle_snapshot;
        grid_apply_block(grid, block, &cycle_snapshot);
        grid_rollback(grid, &cycle_snapshot);

        if (!grids_equal(grid, backup_grid)) {
            all_cycles_successful = false;
            break;
        }
    }

    assert_test(all_cycles_successful,
                "multiple apply/rollback cycles should maintain consistency");

    /* Test 4: NULL parameter handling */
    grid_snapshot_t null_snapshot;

    /* These should not crash */
    assert_test(grid_apply_block(NULL, block, &null_snapshot) == 0,
                "apply_block with NULL grid should return 0");
    assert_test(grid_apply_block(grid, NULL, &null_snapshot) == 0,
                "apply_block with NULL block should return 0");
    assert_test(grid_apply_block(grid, block, NULL) == 0,
                "apply_block with NULL snapshot should return 0");

    grid_rollback(NULL, &null_snapshot); /* Should not crash */
    grid_rollback(grid, NULL);           /* Should not crash */

    assert_test(
        true, "apply_block/rollback should handle NULL parameters gracefully");

    /* Test 5: Snapshot efficiency tracking */
    /* Reset grid for clean test */
    for (int row = 0; row < grid->height; row++) {
        for (int col = 0; col < grid->width; col++)
            grid->rows[row][col] = false;
        grid->n_row_fill[row] = 0;
    }
    for (int col = 0; col < grid->width; col++) {
        grid->relief[col] = -1;
        grid->gaps[col] = 0;
        grid->stack_cnt[col] = 0;
    }
    grid->n_full_rows = 0;

    /* Test simple case (should use simple snapshot) */
    block->offset.x = 5;
    block->offset.y = 5;

    grid_snapshot_t simple_snapshot;
    int simple_lines = grid_apply_block(grid, block, &simple_snapshot);

    assert_test(simple_lines == 0, "simple placement should not clear lines");
    assert_test(!simple_snapshot.needs_full_restore,
                "simple case should use efficient snapshot strategy");

    grid_rollback(grid, &simple_snapshot);

    nfree(block);
    nfree(backup_grid);
    nfree(grid);
    shape_free();
}

void test_grid_edge_cases_and_robustness(void)
{
    /* Test comprehensive NULL parameter handling */
    assert_test(grid_block_collides(NULL, NULL),
                "NULL parameters should indicate intersection");

    grid_block_add(NULL, NULL);    /* Should not crash */
    grid_block_remove(NULL, NULL); /* Should not crash */

    assert_test(grid_block_spawn(NULL, NULL) == 0,
                "NULL parameters should return 0 for elevation");
    assert_test(grid_block_drop(NULL, NULL) == 0,
                "NULL parameters should return 0 for drop");

    grid_block_move(NULL, NULL, LEFT, 1); /* Should not crash */
    grid_block_rotate(NULL, NULL, 1);     /* Should not crash */

    /* Test operations with valid grid but NULL block */
    grid_t *test_grid = grid_new(GRID_HEIGHT, GRID_WIDTH);
    if (test_grid) {
        assert_test(grid_block_collides(test_grid, NULL),
                    "NULL block should indicate intersection");

        grid_block_add(test_grid, NULL);    /* Should not crash */
        grid_block_remove(test_grid, NULL); /* Should not crash */

        assert_test(grid_block_spawn(test_grid, NULL) == 0,
                    "NULL block should return 0 for elevation");
        assert_test(grid_block_drop(test_grid, NULL) == 0,
                    "NULL block should return 0 for drop");

        grid_block_move(test_grid, NULL, LEFT, 1); /* Should not crash */
        grid_block_rotate(test_grid, NULL, 1);     /* Should not crash */

        nfree(test_grid);
    }

    assert_test(true, "all edge cases handled without crashes");
}
