/**
 * auto-tetris: AI-powered Tetris game engine with terminal user interface
 *
 * Key components:
 * - Shape system: Standard 7-piece tetromino set with rotation support
 * - Grid system: Game field with collision detection and line clearing
 * - Block system: Individual piece positioning and movement
 * - AI system: Multi-ply search with evaluation heuristics
 * - TUI system: Terminal-based rendering with color support
 * - Benchmark system: Performance measurement and statistics
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Core Types and Constants
 */

/**
 * Direction enumeration for movement and rotation operations
 *
 * Used for block movement, grid queries, and shape crust calculations.
 * Values correspond to standard geometric directions.
 */
typedef enum {
    BOT,  /**< Downward/bottom direction (gravity) */
    LEFT, /**< Leftward direction */
    TOP,  /**< Upward/top direction */
    RIGHT /**< Rightward direction */
} direction_t;

/**
 * 2D coordinate structure for grid positions
 *
 * Uses unsigned 8-bit integers for memory efficiency.
 * Coordinates are in grid space: (0,0) = bottom-left.
 */
typedef struct {
    uint8_t x, y;
} coord_t;

/** Maximum number of cells in any tetromino piece */
#define MAX_BLOCK_LEN 4

/** Number of standard Tetris shapes (tetrominoes: I, J, L, O, S, T, Z) */
#define NUM_TETRIS_SHAPES 7

/** Default game grid width (wider than standard for AI breathing room) */
#define GRID_WIDTH 14

/** Default game grid height (standard Tetris height) */
#define GRID_HEIGHT 20

/*
 * Shape System
 */

/**
 * Complete tetromino shape definition with all rotations
 *
 * Contains precomputed rotation data, boundary information, and optimization
 * structures for fast collision detection and AI evaluation.
 */
typedef struct {
    int n_rot;           /**< Number of unique rotations (1-4) */
    coord_t rot_wh[4];   /**< Width/height for each rotation */
    int **crust[4][4];   /**< Edge cells for collision detection */
    int crust_len[4][4]; /**< Number of crust cells per direction */
    int crust_flat[4][4][MAX_BLOCK_LEN][2]; /**< Flattened crust data */
    int max_dim_len; /**< Maximum dimension across all rotations */
    int **rot[4];    /**< Cell coordinates for each rotation */
    int rot_flat[4][MAX_BLOCK_LEN][2]; /**< Flattened rotation data */
} shape_t;

/**
 * Initialize the shape system with standard tetromino set
 *
 * Must be called before using any shape-related functions.
 * Creates all 7 standard tetrominoes with rotation data.
 *
 * Return true on success, false on memory allocation failure
 */
bool shape_init(void);

/**
 * Reset the 7-bag random piece generator
 *
 * Forces the next piece selection to start a new shuffled bag.
 * Used primarily for testing and reproducible sequences.
 */
void shape_bag_reset(void);

/**
 * Get shape by index for special effects
 * @index : Shape index (0 to NUM_TETRIS_SHAPES-1)
 *
 * Return Pointer to shape, or NULL if invalid index
 */
shape_t *shape_get(int index);

/**
 * Cleanup all shape system memory
 *
 * Should be called at program exit to free shape resources.
 */
void shape_free(void);

/*
 * Block System
 */

/**
 * Active tetromino piece with position and rotation
 *
 * Represents a falling or placed piece on the grid.
 * Combines shape reference with current position/orientation.
 */
typedef struct {
    coord_t offset; /**< Grid position (bottom-left of bounding box) */
    int rot;        /**< Current rotation index (0 to n_rot-1) */
    shape_t *shape; /**< Reference to tetromino shape definition */
} block_t;

/**
 * Allocate a new block instance
 *
 * Creates an uninitialized block. Must call block_init() before use.
 *
 * Return Pointer to new block, or NULL on allocation failure
 */
block_t *block_new(void);

/**
 * Initialize block with shape and default position
 *
 * Sets block to rotation 0 at position (0,0) with given shape.
 * @b : Block to initialize
 * @s : Shape to assign (can be NULL for empty block)
 */
void block_init(block_t *b, shape_t *s);

/**
 * Get absolute grid coordinates of block cell
 *
 * Calculates the grid position of the i-th cell in the block,
 * accounting for current position and rotation.
 * @b : Block to query
 * @i : Cell index (0 to MAX_BLOCK_LEN-1)
 * @result : Output coordinate (set to (-1,-1) if invalid)
 */
void block_get(block_t *b, int i, coord_t *result);

/**
 * Rotate block by specified amount
 *
 * Positive amounts rotate clockwise, negative counter-clockwise.
 * Rotation wraps around within the shape's valid rotations.
 * @b : Block to rotate
 * @amount : Rotation steps (typically 1 or -1)
 */
void block_rotate(block_t *b, int amount);

/**
 * Move block in specified direction
 *
 * Updates block position without collision checking.
 * Use grid_block_move() for validated movement.
 * @b : Block to move
 * @d : Direction of movement
 * @amount : Distance to move (grid cells)
 */
void block_move(block_t *b, direction_t d, int amount);

/**
 * Get extreme coordinate in specified direction
 *
 * Returns the furthest coordinate of the block in the given direction.
 * Useful for boundary checking and collision detection.
 * @b : Block to query
 * @d : Direction to check
 *
 * Return Extreme coordinate value
 */
int block_extreme(const block_t *b, direction_t d);

/*
 * Grid System
 */

/**
 * Game grid with optimized line clearing and collision detection
 *
 * Maintains both cell occupancy and auxiliary data structures for
 * fast AI evaluation and line clearing operations.
 */
typedef struct {
    bool **rows;         /**< Cell occupancy: rows[y][x] */
    int **stacks;        /**< Column stacks for fast height queries */
    int *stack_cnt;      /**< Number of blocks in each column */
    int *relief;         /**< Highest occupied row per column (-1 if empty) */
    int *n_row_fill;     /**< Number of filled cells per row */
    int *full_rows;      /**< Array of completed row indices */
    int n_full_rows;     /**< Number of currently completed rows */
    int width, height;   /**< Grid dimensions */
    int n_total_cleared; /**< Total lines cleared (lifetime) */
    int n_last_cleared;  /**< Lines cleared in last operation */
    int *gaps;           /**< Empty cells below relief per column */
    uint64_t hash;       /**< Incremental Zobrist hash for fast AI lookup */
} grid_t;

/**
 * Create new game grid
 *
 * Allocates and initializes a grid with specified dimensions.
 * All cells start empty, auxiliary structures are initialized.
 * @height : Grid height in cells
 * @width : Grid width in cells
 *
 * Return Pointer to new grid, or NULL on allocation failure
 */
grid_t *grid_new(int height, int width);

/**
 * Copy grid state to another grid
 *
 * Performs deep copy of all grid data including auxiliary structures.
 * Destination grid must have same dimensions as source.
 * @dest : Destination grid (must be pre-allocated)
 * @src : Source grid to copy from
 */
void grid_copy(grid_t *dest, const grid_t *src);

/**
 * Add block to grid permanently
 *
 * Places all block cells into the grid and updates auxiliary structures.
 * Block should be in final position (use grid_block_drop first).
 * @g : Grid to modify
 * @b : Block to add
 */
void grid_block_add(grid_t *g, block_t *b);

/**
 * Remove block from grid
 *
 * Removes all block cells from grid and updates auxiliary structures.
 * Used for undoing moves during AI search.
 * @g : Grid to modify
 * @b : Block to remove
 */
void grid_block_remove(grid_t *g, block_t *b);

/**
 * Position block at top-center of grid
 *
 * Places block at standard starting position: horizontally centered
 * and elevated above any existing pieces.
 * @g : Grid for positioning reference
 * @b : Block to position
 *
 * Return 1 if positioning successful, 0 if immediate collision
 */
int grid_block_spawn(grid_t *g, block_t *b);

/**
 * Check if block intersects with grid or boundaries
 *
 * Tests whether block at current position would collide with
 * occupied cells or extend outside grid boundaries.
 * @g : Grid to test against
 * @b : Block to test
 *
 * Return true if intersection/collision detected
 */
bool grid_block_collides(grid_t *g, block_t *b);

/**
 * Drop block to lowest valid position
 *
 * Moves block downward until it would collide with grid or bottom.
 * Used for hard drop and AI move execution.
 * @g : Grid for collision testing
 * @b : Block to drop (position modified)
 *
 * Return Number of cells dropped
 */
int grid_block_drop(grid_t *g, block_t *b);

/**
 * Move block with collision validation
 *
 * Attempts to move block in specified direction. If move would cause
 * collision, the block position remains unchanged.
 * @g : Grid for collision testing
 * @b : Block to move
 * @d : Direction of movement
 * @amount : Distance to move
 */
void grid_block_move(grid_t *g, block_t *b, direction_t d, int amount);

/**
 * Rotate block with collision validation
 *
 * Attempts to rotate block. If rotation would cause collision,
 * the block orientation remains unchanged.
 * @g : Grid for collision testing
 * @b : Block to rotate
 * @amount : Rotation steps (positive = clockwise)
 */
void grid_block_rotate(grid_t *g, block_t *b, int amount);

/**
 * Clear completed lines and update grid
 *
 * Removes all completely filled rows, compacts remaining rows downward,
 * and updates all auxiliary data structures.
 * @g : Grid to process
 *
 * Return Number of lines cleared
 */
int grid_clear_lines(grid_t *g);

/**
 * Check if grid is in Tetris-ready state
 *
 * Detects if the grid has a well suitable for a 4-line Tetris clear.
 * A Tetris-ready grid has one column significantly lower than its
 * neighbors, forming a deep well that can accommodate an I-piece.
 * @g : Grid to analyze
 * @well_col : Output parameter for well column index (0-based), or -1 if none
 *
 * Return true if grid is Tetris-ready, false otherwise
 */
bool grid_is_tetris_ready(const grid_t *g, int *well_col);

/*
 * Shape Stream System
 */

/**
 * Shape sequence generator with 7-bag randomization
 *
 * Provides fair tetromino distribution using the "bag" system:
 * each set of 7 pieces contains exactly one of each tetromino type.
 */
typedef struct {
    uint8_t max_len;  /**< Maximum lookahead length */
    int iter;         /**< Current iteration counter */
    bool *defined;    /**< Which preview positions are populated */
    shape_t **stream; /**< Array of upcoming shapes */
} shape_stream_t;

/**
 * Create new shape stream
 *
 * Initializes empty shape sequence. Shapes are generated on-demand
 * using 7-bag randomization for fair distribution.
 *
 * Return Pointer to new stream, or NULL on allocation failure
 */
shape_stream_t *shape_stream_new(void);

/**
 * Preview upcoming shape without consuming it
 *
 * Returns shape at specified preview distance without advancing stream.
 * Index 0 = next shape, 1 = shape after next, etc.
 * @stream : Shape stream to query
 * @idx : Preview distance (0-based)
 *
 * Return Pointer to shape, or NULL if invalid index
 */
shape_t *shape_stream_peek(shape_stream_t *stream, int idx);

/**
 * Get next shape and advance stream
 *
 * Returns the next shape in sequence and advances stream position.
 * This is how pieces are "consumed" during gameplay.
 * @stream : Shape stream to advance
 *
 * Return Pointer to next shape, or NULL on error
 */
shape_t *shape_stream_pop(shape_stream_t *stream);

/*
 * Move Calculation and AI
 */

/**
 * AI move decision with position and rotation
 *
 * Represents the AI's chosen placement for a tetromino piece.
 * Contains final rotation and column position.
 */
typedef struct {
    shape_t *shape; /**< Shape this move applies to */
    int rot;        /**< Target rotation (0 to n_rot-1) */
    int col;        /**< Target column position */
} move_t;

/**
 * Get default AI evaluation weights
 *
 * Returns tuned weights for the AI evaluation function.
 * Caller must free() the returned array.
 *
 * Return Pointer to weight array, or NULL on allocation failure
 */
float *move_defaults(void);

/**
 * Calculate best move for current game state
 *
 * Uses multi-ply search with heuristic evaluation to find optimal
 * placement for the current piece. Considers upcoming pieces for
 * deeper strategic planning.
 * @g : Current game grid
 * @b : Current falling block
 * @ss : Shape stream for preview pieces
 * @w : Evaluation weights array
 *
 * Return Pointer to best move, or NULL if no valid moves
 */
move_t *move_find_best(grid_t *g, block_t *b, shape_stream_t *ss, float *w);

/*
 * Game Logic
 */

/**
 * Run interactive game with AI/human mode switching
 *
 * Main game loop with terminal UI. Supports:
 * - Human play with keyboard controls
 * - AI demonstration mode
 * - Real-time mode switching
 * - Statistics tracking
 *
 * @w : AI evaluation weights
 */
void game_run(float *w);

/*
 * Benchmark System
 */

/**
 * Statistics for a single game run
 *
 * Captures performance metrics for AI evaluation and comparison.
 */
typedef struct {
    int lines_cleared;       /**< Total lines cleared before game over */
    int score;               /**< Final score achieved */
    int pieces_placed;       /**< Number of pieces successfully placed */
    float lcpp;              /**< Lines cleared per piece (efficiency) */
    double game_duration;    /**< Game duration in seconds */
    bool hit_piece_limit;    /**< Whether game ended due to artificial limit */
    float pieces_per_second; /**< AI decision speed metric */
} game_stats_t;

/**
 * Results from multiple benchmark games
 *
 * Aggregates statistics across multiple game runs for performance analysis.
 */
typedef struct {
    game_stats_t *games;       /**< Individual game statistics */
    int num_games;             /**< Total number of games requested */
    game_stats_t avg;          /**< Average performance across games */
    game_stats_t best;         /**< Best single game performance */
    int total_games_completed; /**< Games successfully completed */
    int natural_endings;       /**< Games that ended naturally vs limits */
} bench_results_t;

/**
 * Run single benchmark game without UI
 *
 * Executes one complete game run for performance measurement.
 * Used internally by bench_run_multi() for multiple game analysis.
 * @weights : AI evaluation weights
 * @total_pieces_so_far : Running total for progress tracking
 * @total_expected_pieces : Expected total pieces across all games
 *
 * Return Game statistics for this run
 */
game_stats_t bench_run_single(float *weights,
                              int *total_pieces_so_far,
                              int total_expected_pieces);

/**
 * Run multiple benchmark games for statistical analysis
 *
 * Executes specified number of games and collects performance statistics.
 * Provides progress indication and aggregated results.
 * @weights : AI evaluation weights
 * @num_games : Number of games to run
 *
 * Return Benchmark results with statistics
 */
bench_results_t bench_run_multi(float *weights, int num_games);

/**
 * Print formatted benchmark results
 *
 * Displays comprehensive statistics from benchmark run including
 * averages, best performance, and consistency metrics.
 *
 * @results : Benchmark results to display
 */
void bench_print(const bench_results_t *results);

/*
 * Terminal User Interface
 */

/**
 * Input event types from terminal
 *
 * Represents user input actions for game control.
 */
typedef enum {
    INPUT_INVALID,     /**< No valid input or unknown key */
    INPUT_TOGGLE_MODE, /**< Switch between AI and human mode */
    INPUT_PAUSE,       /**< Pause/unpause game */
    INPUT_QUIT,        /**< Exit game */
    INPUT_ROTATE,      /**< Rotate current piece */
    INPUT_MOVE_LEFT,   /**< Move piece left */
    INPUT_MOVE_RIGHT,  /**< Move piece right */
    INPUT_DROP,        /**< Hard drop piece */
} input_t;

/**
 * Initialize terminal UI system
 *
 * Sets up terminal for game display: raw mode, colors, borders.
 * Must be called before other TUI functions.
 * @g : Grid for layout reference
 */
void tui_setup(const grid_t *g);

/**
 * Build internal display buffer
 *
 * Prepares off-screen buffer with grid state and falling block.
 * Call before tui_render_buffer() for optimal performance.
 * @g : Current grid state
 * @falling_block : Active piece (can be NULL)
 */
void tui_build_buffer(const grid_t *g, block_t *falling_block);

/**
 * Render display buffer to terminal
 *
 * Updates terminal with current display buffer contents.
 * Only redraws changed areas for performance.
 * @g : Grid for layout reference
 */
void tui_render_buffer(const grid_t *g);

/**
 * Force complete display refresh
 *
 * Invalidates display cache and forces full redraw.
 * Use after major state changes or layout updates.
 */
void tui_refresh_force(void);

/**
 * Display preview of next piece
 *
 * Shows upcoming tetromino in sidebar preview area.
 * @b : Block to preview (can be NULL to clear)
 * @color : Display color for the piece
 */
void tui_show_preview(block_t *b, int color);

/**
 * Assign color to placed block
 *
 * Associates color with block cells for persistent display.
 * Call when permanently placing a piece.
 * @b : Block to color
 * @color : ANSI color code (2-7)
 */
void tui_add_block_color(block_t *b, int color);

/**
 * Prepare for line clearing animation
 *
 * Captures current color state before clearing lines.
 * Call before grid_clear_lines() to preserve colors.
 * @g : Grid to preserve colors from
 */
void tui_save_colors(const grid_t *g);

/**
 * Apply preserved colors after line clearing
 *
 * Restores colors to remaining blocks after lines are cleared.
 * Call after grid_clear_lines() to maintain visual consistency.
 * @g : Grid to apply colors to
 */
void tui_restore_colors(const grid_t *g);

/**
 * Force complete display redraw
 *
 * Clears game area and rebuilds entire display.
 * Use sparingly due to performance impact.
 * @g : Grid for layout reference
 */
void tui_force_redraw(const grid_t *g);

/**
 * Perform periodic display maintenance
 *
 * Cleans up display artifacts and refreshes borders.
 * Call periodically during gameplay.
 * @g : Grid for layout reference
 */
void tui_cleanup_display(const grid_t *g);

/**
 * Refresh game borders
 *
 * Redraws the border around the game area.
 * @g : Grid for layout reference
 */
void tui_refresh_borders(const grid_t *g);

/**
 * Update game statistics display
 *
 * Refreshes sidebar with current game statistics.
 * @level : Current game level
 * @points : Current score
 * @lines_cleared : Total lines cleared
 */
void tui_update_stats(int level, int points, int lines_cleared);

/**
 * Update mode indicator display
 *
 * Shows current play mode (AI/Human) in sidebar.
 * @ai_mode : true for AI mode, false for human
 */
void tui_update_mode_display(bool ai_mode);

/**
 * Animate completed line clearing
 *
 * Shows flashing animation for cleared lines.
 * @g : Grid reference
 * @completed_rows : Array of completed row indices
 * @num_completed : Number of rows to animate
 */
void tui_flash_lines(const grid_t *g, int *completed_rows, int num_completed);

/**
 * Display message to user
 *
 * Shows text message in game area (e.g., "Game Over").
 * @g : Grid for positioning reference
 * @msg : Message string to display
 */
void tui_prompt(const grid_t *g, const char *msg);

/**
 * Flush terminal output
 *
 * Ensures all pending output is displayed immediately.
 */
void tui_refresh(void);

/**
 * Get user input with timeout
 *
 * Checks for keyboard input with short timeout for responsive gameplay.
 *
 * Return Input event type
 */
input_t tui_scankey(void);

/**
 * Show falling pieces animation
 *
 * Displays animated falling pieces effect for game over.
 * @g : Grid reference
 */
void tui_animate_gameover(const grid_t *g);

/**
 * Get consistent color for shape type
 *
 * Returns the assigned color for a tetromino shape, assigning
 * one if not already colored.
 * @shape : Shape to get color for
 *
 * Return ANSI color code (2-7)
 */
int tui_get_shape_color(shape_t *shape);

/**
 * Cleanup and restore terminal
 *
 * Restores terminal to original state and cleans up resources.
 * Should be called before program exit.
 */
void tui_quit(void);
