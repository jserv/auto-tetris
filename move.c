#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tetris.h"
#include "utils.h"

#define WORST_SCORE (-FLT_MAX)

/* Multi-ply search depth with configurable optimizations:
 * - Snapshot system: Eliminates expensive grid_copy() operations
 * - State deduplication: Tabu list prevents re-evaluation of identical states
 * - Early pruning: Skip obviously poor candidates before expensive evaluation
 * - SEARCH_DEPTH == 1: Greedy evaluation only
 * - SEARCH_DEPTH == 2: Legacy 2-ply search with tabu optimization
 * - SEARCH_DEPTH >= 3: Alpha-beta search with center-out move ordering +
 *                      beam search + snapshot rollback
 *
 * Performance: 2-ply ≈400 nodes, 3-ply ≈8,000 nodes (~200 with beam=8)
 */
#define SEARCH_DEPTH 3

/* Balanced beam search configuration */
#define BEAM_SIZE 8        /* Smaller beam for faster search */
#define BEAM_SIZE_MAX 14   /* Reasonable crisis expansion */
#define DANGER_THRESHOLD 4 /* Stack height threshold for danger mode */

/* Early pruning thresholds */
/* Rows from top to be considered critical */
#define CRITICAL_HEIGHT_THRESHOLD 3
/* Rows from top for early pruning */
#define TOPOUT_PREVENTION_THRESHOLD 2

/* Enhanced reward system strongly favoring multi-line clears */
#define LINE_CLEAR_BONUS 1.0f     /* Base line clear reward */
#define DOUBLE_CLEAR_BONUS 4.0f   /* 4x bonus for 2-line clear */
#define TRIPLE_CLEAR_BONUS 12.0f  /* 12x bonus for 3-line clear */
#define TETRIS_BONUS 18.0f        /* 18x bonus for 4-line Tetris */
#define HOLE_REDUCTION_BONUS 3.0f /* Strong hole reduction incentive */
#define SURVIVAL_BONUS 1.0f       /* Bonus for surface smoothness in crisis */

/* Tetris setup and well management - strongly incentivized */
#define TETRIS_SETUP_HEIGHT 16   /* Ideal height for Tetris setups */
#define TETRIS_WELL_BONUS 6.0f   /* Stronger bonus for maintaining clean well */
#define I_PIECE_WELL_BONUS 10.0f /* Major bonus for I-piece in well */
#define TETRIS_READY_BONUS 8.0f  /* Higher bonus when board is Tetris-ready */

/* T-spin detection and bonus */
#define T_PIECE_SIGNATURE 0x36 /* Computed signature for T-piece */
#define T_SPIN_BONUS 8.0f      /* Reward for successful T-spin */

/* Combo scoring bonus */
#define COMBO_BONUS 0.5f /* Bonus per combo level */

/* Unified hole penalty system */
#define HOLE_PENALTY 0.7f       /* base cost per hole */
#define HOLE_DEPTH_WEIGHT 0.05f /* extra cost per covered cell above a hole */

/* Surface structure penalties */
#define BUMPINESS_PENALTY 0.02f /* surface roughness */
#define WELL_PENALTY 0.15f      /* deep column penalty */
#define CREVICE_PENALTY 0.20f   /* narrow gap penalty */

/* Penalty per overhang (blocks extending over empty spaces) */
#define OVERHANG_PENALTY 0.2f

/* Transition penalties - Dellacherie & Böhm heuristic */
#define ROW_TRANS_PENALTY 0.18f /* per horizontal transition */
#define COL_TRANS_PENALTY 0.18f /* per vertical transition */

/* Height management optimized for Tetris setups */
#define HEIGHT_PENALTY 0.015f /* Minimal penalty */
/* Increased bonus for strategic Tetris-ready building */
#define STRATEGIC_HEIGHT_BONUS 0.55f
#define STRATEGIC_HEIGHT_START 10 /* Lower threshold for Tetris setup */
#define STRATEGIC_HEIGHT_CAP 17   /* cap remains same */
#define TETRIS_BUILD_HEIGHT 14    /* Optimal height for Tetris preparation */

/* Well management for Tetris opportunities - strong protection */
#define WELL_BLOCK_BASE_PENALTY 3.0f /* Heavy penalty for blocking wells */
#define WELL_BLOCK_DEPTH_FACTOR                  \
    1.0f /* Higher penalty per row of well depth \
          */
#define WELL_ACCESS_BLOCK_PENALTY \
    8.0f /* Severe penalty for blocking well access */
#define WELL_MAINTENANCE_BONUS 2.0f /* Strong bonus for keeping wells clear */
#define PERFECT_WELL_DEPTH 4        /* Ideal well depth for Tetris */

/* Terminal position penalty when stack hits ceiling */
#define TOPOUT_PENALTY 50000.0f

/* Defensive configuration for maximum survival */
#define EARLY_GAME_PIECES 1000      /* Very short early game period */
#define CRISIS_MODE_THRESHOLD 10    /* Ultra-low crisis threshold */
#define EMERGENCY_MODE_THRESHOLD 13 /* Much lower emergency threshold */
#define DESPERATE_MODE_THRESHOLD 15 /* Lower desperate mode threshold */
#define PANIC_MODE_THRESHOLD 17     /* New panic mode for extreme situations */

/* Crisis management - consolidated constants */
#define CRITICAL_HOLE_THRESHOLD 5  /* Hole threshold for emergency mode */
#define VALLEY_DEPTH_THRESHOLD 6   /* Deep valley detection */
#define SURFACE_CHAOS_THRESHOLD 8  /* Surface irregularity threshold */
#define EMERGENCY_CLEAR_BONUS 3.0f /* Bonus for clearing lines in crisis */
#define VALLEY_FILL_BONUS 1.0f     /* Reward for filling deep valleys */

/* Human-to-AI handoff detection and recovery */
#define HANDOFF_CHAOS_THRESHOLD 12   /* Detecting chaotic handoff state */
#define HANDOFF_RECOVERY_BONUS 2.0f  /* Bonus for recovery from handoff */
#define INITIAL_ASSESSMENT_PIECES 20 /* Pieces to assess handoff difficulty */

/* Time management for iterative deepening */
#define DEFAULT_TIME_LIMIT_MS 100 /* Default time limit in milliseconds */
#define MIN_TIME_PER_DEPTH_MS 5   /* Minimum time to spend per depth level */

/* Main evaluation cache for complete scores */
#define EVAL_CACHE_SIZE 8192 /* 8K entries ≈64 KiB for better hit rates */

struct eval_cache_entry {
    uint64_t key; /* 64-bit hash: grid + weights + piece context */
    float val;    /* cached evaluation */
};

/* Specialized metrics cache for expensive computations only
 * Smaller, focused cache for maximum hit rate on hot metrics
 */
#define METRICS_CACHE_SIZE 4096 /* 4K entries ≈48 KiB, L2-resident */
#define METRICS_CACHE_MASK (METRICS_CACHE_SIZE - 1)

struct metrics_entry {
    uint64_t grid_key;   /* Grid-only hash (weight-independent) */
    float hole_penalty;  /* Cached hole penalty with depth */
    uint16_t bumpiness;  /* Cached surface roughness */
    uint16_t row_trans;  /* Cached row transitions */
    uint16_t col_trans;  /* Cached column transitions */
    uint16_t well_depth; /* Cached well depth */
    uint16_t crevices;   /* Cached crevice count */
    uint16_t overhangs;  /* Cached overhang count */
};

#if SEARCH_DEPTH >= 2
/* Tabu list for avoiding duplicate grid state evaluations */
#define TABU_SIZE 128 /* Power of 2 for fast masking */
#endif

/* Evolved weights (Fitness: 1269.00) */
static const float default_weights[N_FEATIDX] = {
    [FEATIDX_RELIEF_MAX] = -1.5285f, [FEATIDX_RELIEF_AVG] = -1.8356f,
    [FEATIDX_RELIEF_VAR] = -0.4441f, [FEATIDX_GAPS] = -2.1800f,
    [FEATIDX_OBS] = -1.2554f,        [FEATIDX_DISCONT] = -0.5567f,
    [FEATIDX_CREVICES] = -0.6381f,
};


/* Board reuse pool for eliminating malloc/free overhead */
#define GRID_POOL_SIZE 8 /* Pre-allocated grids for reuse */

typedef struct {
    grid_t grids[GRID_POOL_SIZE];     /* Pool of reusable grids */
    bool grid_in_use[GRID_POOL_SIZE]; /* Track which grids are allocated */
    int pool_initialized;             /* Whether pool has been set up */
} grid_pool_t;

/* Move cache optimized for snapshot-based search
 * Eliminates expensive grid copying with efficient snapshot/rollback operations
 */
typedef struct {
    grid_t working_grid;    /* Primary working grid for snapshot operations */
    block_t *search_blocks; /* Block instances for testing placements */
    move_t *cand_moves;     /* Best moves found at each search depth */
    int size;               /* Number of cached items (equals search depth) */
    bool initialized;       /* Whether cache has been set up */
} move_cache_t;

/* Beam search candidate structure */
typedef struct {
    int rot;       /* Rotation for this candidate */
    int col;       /* Column position for this candidate */
    int lines;     /* Lines cleared by this placement */
    float shallow; /* Shallow evaluation score */
} beam_candidate_t;

/* Beam search performance monitoring */
typedef struct {
    int positions_evaluated;   /* Total positions considered */
    int beam_hits;             /* Best moves found in beam */
    int beam_misses;           /* Best moves outside beam */
    float avg_beam_score_diff; /* Average score difference */
    int adaptive_expansions;   /* Times beam was expanded */
    int early_pruned;          /* Candidates pruned early */
} beam_stats_t;

/* Move filtering performance statistics */
typedef struct {
    int total_candidates;    /* Total move candidates considered */
    int collision_filtered;  /* Filtered by fast collision check */
    int height_filtered;     /* Filtered by critical height checks */
    int width_filtered;      /* Filtered by width-based heuristics */
    int structure_filtered;  /* Filtered by structural problem detection */
    int piece_filtered;      /* Filtered by piece-specific rules */
    int evaluated;           /* Candidates that passed all filters */
    float filter_efficiency; /* Percentage of candidates filtered out */
} filter_stats_t;

/* Search performance statistics */
typedef struct {
    int depth_2_selections;    /* Times optimized search was used */
    int depth_3_selections;    /* Times full search was used */
    int depth_4_selections;    /* Times emergency deep search was used */
    int total_evaluations;     /* Total search invocations */
    int max_height_seen;       /* Highest stack observed */
    int piece_count;           /* Current piece number */
    int snapshots_used;        /* Number of snapshot operations */
    int rollbacks_used;        /* Number of rollback operations */
    float snapshot_efficiency; /* Ratio of simple vs full snapshots */
} depth_stats_t;

/* Time management for iterative deepening */
typedef struct {
    clock_t start_time;     /* Search start time */
    clock_t time_limit;     /* Maximum time allowed (in clock ticks) */
    bool time_limited;      /* Whether search has time limit */
    int early_terminations; /* Number of searches terminated early */
    int completed_depths;   /* Depths completed before timeout */
} time_manager_t;

/* Move ordering data structures for alpha-beta optimization */
#define MAX_SEARCH_DEPTH 8
#define KILLER_SLOTS 2

typedef struct {
    /* History heuristic: tracks successful moves by depth */
    int history_table[MAX_SEARCH_DEPTH][GRID_WIDTH][4];

    /* Killer moves: best moves from sibling nodes */
    struct {
        int col, rot;
        bool valid;
    } killer_moves[MAX_SEARCH_DEPTH][KILLER_SLOTS];

    /* Principal variation: best line from previous search */
    struct {
        int moves[MAX_SEARCH_DEPTH * 2]; /* [col, rot, col, rot, ...] */
        int length;
        bool valid;
    } principal_variation;

    /* Move ordering statistics */
    struct {
        int total_cutoffs;
        int history_cutoffs;
        int killer_cutoffs;
        int pv_cutoffs;
        int total_nodes;
        float cutoff_rate; /* Percentage of nodes that caused beta cutoff */
    } stats;
} move_ordering_t;

/* Cached weights hash */
typedef struct {
    const float *ptr; /* Pointer to weights array (identity check) */
    uint64_t hash;    /* Cached hash of weights */
    bool valid;       /* Whether cache entry is valid */
} weights_cache_t;

/* Single aggregate that owns every piece of global state */
typedef struct {
    struct eval_cache_entry eval_cache[EVAL_CACHE_SIZE];
    struct metrics_entry metrics_cache[METRICS_CACHE_SIZE];

#if SEARCH_DEPTH >= 2
    uint64_t tabu_seen[TABU_SIZE];
    uint8_t tabu_age[TABU_SIZE];
    uint8_t tabu_current_age;
#endif

    grid_pool_t grid_pool;
    move_cache_t move_cache;
    beam_stats_t beam_stats;
    filter_stats_t filter_stats;
    depth_stats_t depth_stats;
    move_ordering_t move_ordering;
    weights_cache_t weights_cache;
    time_manager_t time_manager;
    bool cleanup_registered;
} move_globals_t;

static move_globals_t G = {0};

/* One-line shims — existing code keeps the same names */
#define eval_cache (G.eval_cache)
#define metrics_cache (G.metrics_cache)
#define grid_pool (G.grid_pool)
#define move_cache (G.move_cache)
#define beam_stats (G.beam_stats)
#define filter_stats (G.filter_stats)
#define depth_stats (G.depth_stats)
#define move_ordering (G.move_ordering)
#define weights_cache (G.weights_cache)
#define time_manager (G.time_manager)
#define cleanup_registered (G.cleanup_registered)
#if SEARCH_DEPTH >= 2
#define tabu_seen (G.tabu_seen)
#define tabu_age (G.tabu_age)
#define tabu_current_age (G.tabu_current_age)
#endif

/* Forward declarations */
static void cache_cleanup(void);
static int get_crevices(const grid_t *g);
static bool is_chaotic_handoff(const grid_t *g);
static float apply_structural_penalties(const grid_t *g,
                                        float crisis_multiplier,
                                        float setup_discount);
static int count_complete_rows(const grid_t *g, int min_row, int max_row);
static float evaluate_tetris_potential(const grid_t *g);
static float ab_search_snapshot(grid_t *working_grid,
                                const shape_stream_t *shapes,
                                const float *weights,
                                int depth,
                                int piece_index,
                                float alpha,
                                float beta,
                                int combo_count);

/* Time management functions */
static void time_manager_init(int time_limit_ms);
static bool time_manager_should_stop(void);
static void time_manager_reset(void);

/* Grid pool management functions */
static bool grid_pool_init(const grid_t *template_grid);
static grid_t *grid_pool_acquire(void);
static void grid_pool_release(grid_t *grid);
static void grid_pool_cleanup(void);
static void grid_fast_copy(grid_t *dest, const grid_t *src);

/* Time management implementation */
static void time_manager_init(int time_limit_ms)
{
    time_manager.start_time = clock();
    if (time_limit_ms > 0) {
        time_manager.time_limit =
            (clock_t) ((time_limit_ms * CLOCKS_PER_SEC) / 1000);
        time_manager.time_limited = true;
    } else {
        time_manager.time_limited = false;
        time_manager.time_limit = 0;
    }
    time_manager.completed_depths = 0;
}

static bool time_manager_should_stop(void)
{
    if (!time_manager.time_limited)
        return false;

    clock_t elapsed = clock() - time_manager.start_time;
    return elapsed >= time_manager.time_limit;
}

static void time_manager_reset(void)
{
    time_manager.start_time = 0;
    time_manager.time_limit = 0;
    time_manager.time_limited = false;
    time_manager.early_terminations = 0;
    time_manager.completed_depths = 0;
}

/* Fast memory copy optimized for grid structures */
static void grid_fast_copy(grid_t *dest, const grid_t *src)
{
    if (!dest || !src)
        return;

    /* Save destination pointers */
    int **dest_stacks = dest->stacks;
    int *dest_relief = dest->relief;
    int *dest_gaps = dest->gaps;
    int *dest_stack_cnt = dest->stack_cnt;
    int *dest_full_rows = dest->full_rows;

    /* Copy entire structure (including inline rows array) */
    *dest = *src;

    /* Restore destination pointers */
    dest->stacks = dest_stacks;
    dest->relief = dest_relief;
    dest->gaps = dest_gaps;
    dest->stack_cnt = dest_stack_cnt;
    dest->full_rows = dest_full_rows;

    /* Copy arrays using fast memcpy */
    if (src->relief && dest->relief)
        memcpy(dest->relief, src->relief, src->width * sizeof(*dest->relief));
    if (src->gaps && dest->gaps)
        memcpy(dest->gaps, src->gaps, src->width * sizeof(*dest->gaps));
    if (src->stack_cnt && dest->stack_cnt)
        memcpy(dest->stack_cnt, src->stack_cnt,
               src->width * sizeof(*dest->stack_cnt));
    if (src->full_rows && dest->full_rows)
        memcpy(dest->full_rows, src->full_rows,
               src->height * sizeof(*dest->full_rows));

    /* Copy per-column stacks if they exist */
    if (src->stacks && dest->stacks) {
        for (int c = 0; c < src->width; c++) {
            if (src->stacks[c] && dest->stacks[c]) {
                memcpy(dest->stacks[c], src->stacks[c],
                       src->height * sizeof(*dest->stacks[c]));
            }
        }
    }
}

/* Initialize grid pool with pre-allocated grids */
static bool grid_pool_init(const grid_t *template_grid)
{
    if (grid_pool.pool_initialized || !template_grid)
        return grid_pool.pool_initialized;

    for (int i = 0; i < GRID_POOL_SIZE; i++) {
        grid_pool.grid_in_use[i] = false;
        grid_t *grid = &grid_pool.grids[i];
        *grid = *template_grid;

        /* Allocate arrays */
        grid->relief =
            ncalloc(template_grid->width, sizeof(*grid->relief), NULL);
        grid->gaps = ncalloc(template_grid->width, sizeof(*grid->gaps), NULL);
        grid->stack_cnt =
            ncalloc(template_grid->width, sizeof(*grid->stack_cnt), NULL);
        grid->full_rows =
            ncalloc(template_grid->height, sizeof(*grid->full_rows), NULL);
        grid->stacks =
            ncalloc(template_grid->width, sizeof(*grid->stacks), NULL);

        if (!grid->relief || !grid->gaps || !grid->stack_cnt ||
            !grid->full_rows || !grid->stacks) {
            grid_pool_cleanup();
            return false;
        }

        /* Allocate per-column stacks */
        for (int c = 0; c < template_grid->width; c++) {
            grid->stacks[c] = ncalloc(template_grid->height,
                                      sizeof(*grid->stacks[c]), grid->stacks);
            if (!grid->stacks[c]) {
                grid_pool_cleanup();
                return false;
            }
        }
    }

    grid_pool.pool_initialized = true;
    return true;
}

/* Acquire a grid from the pool */
static grid_t *grid_pool_acquire(void)
{
    if (!grid_pool.pool_initialized)
        return NULL;

    for (int i = 0; i < GRID_POOL_SIZE; i++) {
        if (!grid_pool.grid_in_use[i]) {
            grid_pool.grid_in_use[i] = true;
            return &grid_pool.grids[i];
        }
    }
    return NULL;
}

/* Release a grid back to the pool */
static void grid_pool_release(grid_t *grid)
{
    if (!grid || !grid_pool.pool_initialized)
        return;

    for (int i = 0; i < GRID_POOL_SIZE; i++) {
        if (&grid_pool.grids[i] == grid) {
            grid_pool.grid_in_use[i] = false;
            return;
        }
    }
}

/* Cleanup grid pool */
static void grid_pool_cleanup(void)
{
    if (!grid_pool.pool_initialized)
        return;

    for (int i = 0; i < GRID_POOL_SIZE; i++) {
        grid_t *grid = &grid_pool.grids[i];
        nfree(grid->stacks);
        nfree(grid->relief);
        nfree(grid->gaps);
        nfree(grid->stack_cnt);
        nfree(grid->full_rows);
        grid_pool.grid_in_use[i] = false;
    }
    grid_pool.pool_initialized = false;
}

/* Fast cell access helper for packed grid */
static inline bool move_cell_occupied(const grid_t *g, int x, int y)
{
    return (g->rows[y] >> x) & 1ULL;
}

/* T-spin detection function
 *
 * Checks for the classic T-spin condition: at least 3 of the 4 corners
 * of the T-piece's 3x3 bounding box must be filled after placement.
 * This is the standard modern Tetris T-spin recognition rule.
 */
static bool is_t_spin(const grid_t *g, const block_t *block)
{
    if (!g || !block || !block->shape)
        return false;

    /* Only T-pieces can perform T-spins */
    if (block->shape->sig != T_PIECE_SIGNATURE)
        return false;

    /* Find the center of the T-piece to determine the corners.
     * The T-piece has a 3x3 bounding box, so center is at offset (1,1)
     */
    int cx = block->offset.x + 1;
    int cy = block->offset.y + 1;

    /* Define the four corner positions relative to the grid */
    int corners_occupied = 0;
    if (cx > 0 && cy > 0 && move_cell_occupied(g, cx - 1, cy - 1))
        corners_occupied++;
    if (cx < g->width - 1 && cy > 0 && move_cell_occupied(g, cx + 1, cy - 1))
        corners_occupied++;
    if (cx > 0 && cy < g->height - 1 && move_cell_occupied(g, cx - 1, cy + 1))
        corners_occupied++;
    if (cx < g->width - 1 && cy < g->height - 1 &&
        move_cell_occupied(g, cx + 1, cy + 1))
        corners_occupied++;

    return corners_occupied >= 3;
}

/* Fast shape coordinate checking for optimization
 *
 * Checks if a grid coordinate is part of the current falling block.
 * Used to skip cells occupied by the active piece during evaluation.
 * Returns true if (x,y) is occupied by any cell of the block.
 */
static inline bool is_block_coordinate(const block_t *b, int x, int y)
{
    if (!b || !b->shape)
        return false;

    /* Use flattened rotation data for performance */
    int rot = b->rot;
    if (rot < 0 || rot >= 4)
        return false;

    /* Cache base coordinates to reduce repeated arithmetic */
    const int (*coords)[2] = (const int (*)[2]) b->shape->rot_flat[rot];
    int base_x = b->offset.x;
    int base_y = b->offset.y;

    /* Check all cells with optimized loop */
    for (int i = 0; i < MAX_BLOCK_LEN; i++) {
        int cell_x = coords[i][0];
        int cell_y = coords[i][1];

        /* Skip invalid coordinates marked as -1 */
        if (cell_x == -1)
            continue;

        /* Direct comparison without intermediate variables */
        if (base_x + cell_x == x && base_y + cell_y == y)
            return true;
    }
    return false;
}

/* Ensure cleanup is registered when move module is first used */
static void ensure_cleanup(void)
{
    if (!cleanup_registered) {
        atexit(cache_cleanup);
        cleanup_registered = true;
    }
}

/* Return default weights */
float *move_defaults()
{
    ensure_cleanup();

    float *weights = malloc(sizeof(default_weights));
    if (!weights)
        return NULL;

    memcpy(weights, default_weights, sizeof(default_weights));
    return weights;
}

/* Fast collision pre-check to avoid expensive grid operations
 *
 * Performs lightweight collision detection using cached relief data
 * before expensive grid_block_drop and grid_apply_block operations.
 * Returns true if placement would definitely collide.
 */
static bool quick_collision_check(const grid_t *g, const block_t *test_block)
{
    if (!g || !test_block || !test_block->shape || !g->relief)
        return true;

    const int rot = test_block->rot;
    const int base_x = test_block->offset.x;
    const int base_y = test_block->offset.y;

    /* Use flattened rotation data for performance */
    const int (*coords)[2] =
        (const int (*)[2]) test_block->shape->rot_flat[rot];

    /* Check each cell of the piece against relief data */
    for (int i = 0; i < MAX_BLOCK_LEN; i++) {
        int cell_x = coords[i][0];
        int cell_y = coords[i][1];

        /* Skip invalid coordinates marked as -1 */
        if (cell_x == -1)
            continue;

        int world_x = base_x + cell_x;
        int world_y = base_y + cell_y;

        /* Bounds check */
        if (world_x < 0 || world_x >= g->width || world_y < 0)
            return true;

        /* Quick relief-based collision check */
        if (world_y <= g->relief[world_x])
            return true;
    }

    return false;
}

/* Enhanced viability filtering with ai/ai.c inspired optimizations
 *
 * Multi-stage filtering system that eliminates poor candidates before
 * expensive evaluation operations. Implements fast heuristics similar
 * to the reference ai/ai.c implementation for maximum performance.
 */
static bool should_skip_evaluation(const grid_t *g, const block_t *test_block)
{
    if (!g || !test_block || !test_block->shape)
        return true;

    /* Track total candidates for performance analysis */
    filter_stats.total_candidates++;

    /* Stage 1: Fast collision pre-check */
    if (quick_collision_check(g, test_block)) {
        filter_stats.collision_filtered++;
        return true;
    }

    const int landing_col = test_block->offset.x;
    const int piece_width = test_block->shape->rot_wh[test_block->rot].x;
    const int piece_height = test_block->shape->rot_wh[test_block->rot].y;

    /* Stage 2: Board state analysis (cached from previous call) */
    static int cached_max_relief = -1;
    static int cached_critical_cols = -1;
    static uint64_t cached_grid_hash = 0;

    int max_relief, critical_cols;
    if (cached_grid_hash == g->hash) {
        /* Use cached values for same grid state */
        max_relief = cached_max_relief;
        critical_cols = cached_critical_cols;
    } else {
        /* Compute and cache board state metrics */
        max_relief = 0;
        critical_cols = 0;
        for (int x = 0; x < g->width; x++) {
            if (g->relief[x] > max_relief)
                max_relief = g->relief[x];
            if (g->relief[x] >= g->height - CRITICAL_HEIGHT_THRESHOLD)
                critical_cols++;
        }
        cached_max_relief = max_relief;
        cached_critical_cols = critical_cols;
        cached_grid_hash = g->hash;
    }

    /* Stage 3: Critical height filtering - be more careful in crisis */
    if (max_relief >= g->height - CRITICAL_HEIGHT_THRESHOLD) {
        /* In crisis mode, be more lenient with filtering */
        bool in_emergency = (max_relief >= EMERGENCY_MODE_THRESHOLD);

        /* Check if piece would fit at all */
        if (landing_col >= 0 && landing_col < g->width) {
            int estimated_landing = g->relief[landing_col] + piece_height;
            /* Only filter if it definitely won't fit */
            if (estimated_landing >= g->height && !in_emergency) {
                filter_stats.height_filtered++;
                return true;
            }
        }

        /* In emergency, consider all moves that might help */
        if (max_relief >= g->height - TOPOUT_PREVENTION_THRESHOLD &&
            !in_emergency) {
            int piece_top = g->relief[landing_col] + piece_height;
            /* Only filter moves that make things worse */
            if (piece_top > max_relief) {
                filter_stats.height_filtered++;
                return true;
            }
        }
    }

    /* Stage 4: Width-based filtering for tight spaces */
    if (critical_cols >= g->width / 2 && piece_width >= 3) {
        int piece_left = landing_col;
        int piece_right = landing_col + piece_width - 1;

        int affected_critical = 0;
        for (int x = MAX(0, piece_left); x <= MIN(g->width - 1, piece_right);
             x++) {
            if (g->relief[x] >= g->height - CRITICAL_HEIGHT_THRESHOLD)
                affected_critical++;
        }

        if (affected_critical >= 2) {
            filter_stats.width_filtered++;
            return true;
        }
    }

    /* Stage 5: Structural viability - adjusted for crisis */

    /* Check if we're in crisis mode */
    bool in_crisis = (max_relief >= CRISIS_MODE_THRESHOLD);

    /* Skip moves that create obvious structural problems */
    if (landing_col > 0 && landing_col < g->width - 1 && !in_crisis) {
        int left_height = g->relief[landing_col - 1] + 1;
        int center_height = g->relief[landing_col] + piece_height;
        int right_height = g->relief[landing_col + 1] + 1;

        /* In crisis, be more lenient with structural issues */
        int well_depth_threshold = in_crisis ? 5 : 3;
        int well_depth = MIN(left_height, right_height) - center_height;
        if (well_depth > well_depth_threshold) {
            filter_stats.structure_filtered++;
            return true;
        }

        /* Skip moves that create extreme height differences */
        int variance_threshold = in_crisis ? g->height / 2 : g->height / 3;
        int height_variance = abs(left_height - center_height) +
                              abs(center_height - right_height);
        if (height_variance > variance_threshold) {
            filter_stats.structure_filtered++;
            return true;
        }
    }

    /* Stage 6: Piece-specific filtering */
    bool is_I_piece = (test_block->shape->rot_wh[0].x == 4);
    bool is_O_piece = (test_block->shape->rot_wh[0].x == 2 &&
                       test_block->shape->rot_wh[0].y == 2);

    /* I-pieces: Special handling for Tetris opportunities */
    if (is_I_piece) {
        int well_col = -1;
        bool tetris_ready = grid_is_tetris_ready(g, &well_col);

        if (tetris_ready) {
            /* When Tetris-ready, strongly prefer vertical I in well */
            if (test_block->rot == 1) { /* Vertical I */
                if (landing_col != well_col) {
                    /* Filter out vertical I-pieces not in the well */
                    filter_stats.piece_filtered++;
                    return true;
                }
            } else if (test_block->rot == 0) { /* Horizontal I */
                /* Allow horizontal I only if it helps clear lines */
                bool helps_clear = false;
                for (int x = landing_col; x < landing_col + 4 && x < g->width;
                     x++) {
                    if (x >= 0 && g->relief[x] >= g->height - 4) {
                        helps_clear = true;
                        break;
                    }
                }
                if (!helps_clear && max_relief > g->height * 0.6f) {
                    filter_stats.piece_filtered++;
                    return true;
                }
            }
        } else if (test_block->rot == 0) { /* Horizontal I without well */
            bool can_clear_line = false;
            for (int x = landing_col; x < landing_col + 4 && x < g->width;
                 x++) {
                if (x >= 0 && g->relief[x] >= g->height * 0.7f) {
                    can_clear_line = true;
                    break;
                }
            }
            /* In mid-to-late game, prioritize I-pieces for line clearing */
            if (max_relief > g->height / 2 && !can_clear_line) {
                filter_stats.piece_filtered++;
                return true;
            }
        }
    }

    /* O-pieces: avoid creating unreachable gaps */
    if (is_O_piece) {
        if (landing_col >= 0 && landing_col + 1 < g->width) {
            int left_relief = g->relief[landing_col];
            int right_relief = g->relief[landing_col + 1];
            /* Skip if O-piece would create isolated high spots */
            if (abs(left_relief - right_relief) > 2) {
                filter_stats.piece_filtered++;
                return true;
            }
        }
    }

    /* Candidate passed all filters */
    filter_stats.evaluated++;
    return false; /* Continue with full evaluation */
}

/* Adaptive search depth optimization
 *
 * Hybrid strategy balances speed and quality:
 * - Early game + safe stacks → 2-ply search (faster)
 * - Late game or danger → 3-ply search (full strength)
 */

/* Uses adaptive depth selection to balance speed and quality:
 * early positions use optimized search, critical positions use full depth.
 */
static inline int dynamic_search_depth(const grid_t *g)
{
    if (!g || !g->relief) {
        depth_stats.total_evaluations++;
        depth_stats.depth_3_selections++;
        return SEARCH_DEPTH;
    }

    /* Increment piece counter */
    depth_stats.piece_count++;
    depth_stats.total_evaluations++;

    /* Fast height and hole calculation only */
    int max_h = 0;
    int hole_count = 0;

    for (int x = 0; x < g->width; x++) {
        if (g->relief[x] > max_h)
            max_h = g->relief[x];
        hole_count += g->gaps[x];
    }

    /* Skip surface variance calculation for speed */

    /* Update height statistics */
    if (max_h > depth_stats.max_height_seen)
        depth_stats.max_height_seen = max_h;

    /* Check for chaotic handoff requiring special attention */
    bool is_chaotic = is_chaotic_handoff(g);

    /* Multi-level emergency depth selection */
    if (max_h >= PANIC_MODE_THRESHOLD) {
        depth_stats.depth_4_selections++;
        /* Panic mode: Maximum possible search depth */
        return MIN(SEARCH_DEPTH + 2, 5); /* Cap at 5 for panic mode */
    }

    if (max_h >= DESPERATE_MODE_THRESHOLD || is_chaotic) {
        depth_stats.depth_4_selections++;
        /* Desperate situations need enhanced lookahead */
        return MIN(SEARCH_DEPTH + 1, 4); /* Cap at 4 for desperate mode */
    }

    /* Crisis detection for deeper search. Use deeper search if the stack is
     * very high, or if the stack is moderately high and riddled with holes.
     */
    if (max_h >= EMERGENCY_MODE_THRESHOLD ||
        (hole_count > CRITICAL_HOLE_THRESHOLD &&
         max_h >= CRISIS_MODE_THRESHOLD)) {
        depth_stats.depth_4_selections++;
        /* Emergency mode uses enhanced depth */
        return MIN(SEARCH_DEPTH + 1, 4);
    }

    /* Crisis mode */
    if (max_h >= CRISIS_MODE_THRESHOLD) {
        /* Always use full depth in crisis - survival over speed */
        depth_stats.depth_3_selections++;
        return SEARCH_DEPTH;
    }

    /* Late game: Simple decisions */
    if (depth_stats.piece_count > EARLY_GAME_PIECES) {
        /* Only use full depth for severe problems */
        if (hole_count > 12) {
            depth_stats.depth_3_selections++;
            return SEARCH_DEPTH;
        }
        /* Most late game uses fast search */
        depth_stats.depth_2_selections++;
        return 2;
    }

    /* Normal mode: Prefer speed */
    int threshold = (g->height * 4) / 5; /* Keep original threshold */

    if (max_h < threshold && hole_count < 8) {
        /* Clean board - use fast search */
        depth_stats.depth_2_selections++;
        return 2;
    }

    /* Default to standard depth */
    depth_stats.depth_3_selections++;
    return SEARCH_DEPTH;
}

/* Fast hash computation for weights array with caching */
static uint64_t hash_weights(const float *weights)
{
    if (!weights)
        return 0;

    /* Fast path: same weights pointer as last call? */
    if (weights_cache.valid && weights_cache.ptr == weights)
        return weights_cache.hash;

    /* Compute hash for new weights */
    uint64_t hash = 0x9e3779b97f4a7c15ULL; /* Initial seed */

    for (int i = 0; i < N_FEATIDX; i++) {
        /* Convert float to uint32 for consistent hashing */
        union {
            float f;
            uint32_t i;
        } conv = {.f = weights[i]};
        hash ^= conv.i + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    }

    /* Cache result */
    weights_cache.ptr = weights;
    weights_cache.hash = hash;
    weights_cache.valid = true;

    return hash;
}

/* Generate piece context hash for improved cache discrimination */
static uint64_t hash_piece_context(const shape_t *shape,
                                   int rotation,
                                   int column)
{
    if (!shape)
        return 0;

    /* Combine shape signature, rotation, and column into discriminating hash */
    uint64_t hash = shape->sig;             /* Shape geometry signature */
    hash = (hash << 8) ^ (rotation & 0xFF); /* Add rotation info */
    hash = (hash << 8) ^ (column & 0xFF);   /* Add column info */

    /* Mix bits for better distribution */
    hash ^= hash >> 33;
    hash *= 0xff51afd7ed558ccdULL;
    hash ^= hash >> 33;
    hash *= 0xc4ceb9fe1a85ec53ULL;
    hash ^= hash >> 33;

    return hash;
}

/* Calculate grid features, optionally returning bumpiness */
static void calc_features(const grid_t *restrict g,
                          float *restrict features,
                          int *restrict bump_out)
{
    if (!g || !features) {
        if (features) {
            for (int i = 0; i < N_FEATIDX; i++)
                features[i] = 0.0f;
        }
        if (bump_out)
            *bump_out = 0;
        return;
    }

    const int width = g->width;
    float sum = 0.0f, max = 0.0f;
    int discont = -1, last_height = -1;
    int gaps = 0, obs = 0;
    int bump = 0;

    /* First pass: collect height sum, max, gaps, occupancy, discontinuities,
     * bumpiness
     */
    for (int i = 0; i < width; i++) {
        const int height = g->relief[i] + 1; /* column height */
        const int cgaps = g->gaps[i];        /* holes in column */

        if (height > max)
            max = height;
        sum += height;
        discont += (int) (last_height != height);

        if (last_height >= 0)
            bump += abs(height - last_height);
        last_height = height;

        gaps += cgaps;
        obs += height - cgaps;
    }

    const float avg = sum / width;

    /* Second pass: calculate variance using known average */
    float var = 0.0f;
    for (int i = 0; i < width; i++) {
        const float diff = avg - (g->relief[i] + 1);
        var += diff * diff;
    }

    /* Store calculated features */
    features[FEATIDX_RELIEF_MAX] = max;
    features[FEATIDX_RELIEF_AVG] = avg;
    features[FEATIDX_RELIEF_VAR] = var;
    features[FEATIDX_DISCONT] = discont;
    features[FEATIDX_GAPS] = gaps;
    features[FEATIDX_OBS] = obs;
    features[FEATIDX_CREVICES] = (float) get_crevices(g);

    if (bump_out)
        *bump_out = bump;
}

/* Advanced Move Ordering for Alpha-Beta Pruning Optimization
 *
 * Effective move ordering is crucial for alpha-beta pruning performance.
 * The key insight: search the most promising moves first to maximize cutoffs.
 *
 * Implemented ordering heuristics (in priority order):
 *
 * 1. History Heuristic: Moves that caused cutoffs at the same depth before
 *    - Maintains per-depth statistics of good moves
 *    - Statistical learning improves over game progression
 *    - ~15-20% more beta cutoffs in typical game tree search
 *
 * 2. Killer Moves: Best moves from sibling nodes at same depth
 *    - Exploits position similarity in game trees
 *    - Two killer slots per depth level (primary/secondary)
 *    - Particularly effective in tactical positions
 *
 * 3. Principal Variation (PV): Continuation of best line from previous
 * iteration
 *    - In iterative deepening, previous iteration's best move is tried first
 *    - Often the true best move, causing immediate cutoffs
 *    - Critical for maintaining search stability
 *
 * 4. Center-Column Heuristic: Geometric placement preference
 *    - Center columns often provide more tactical options
 *    - Fallback when no historical data available
 *    - Original heuristic, now enhanced with position-specific scoring
 */
static int centre_out_order(int order[], int width)
{
    /* Static cache - width is typically 10-14 in standard play */
    static int cache[GRID_WIDTH];
    static int cached_width = -1;
    static int cached_count = 0;

    /* Bounds check */
    if (width <= 0 || width > GRID_WIDTH)
        return 0;

    /* Hot path: return cached result if width matches */
    if (width == cached_width) {
        memcpy(order, cache, cached_count * sizeof(int));
        return cached_count;
    }

    /* Cold path: compute and cache new ordering */
    int centre = width / 2;
    int idx = 0;

    cache[idx] = centre;
    order[idx] = centre;
    idx++;

    for (int off = 1; off <= centre; ++off) {
        int right = centre + off;
        int left = centre - off;

        if (right < width) {
            cache[idx] = right;
            order[idx] = right;
            idx++;
        }
        if (left >= 0) {
            cache[idx] = left;
            order[idx] = left;
            idx++;
        }
    }

    /* Update cache metadata */
    cached_width = width;
    cached_count = idx;

    return idx;
}

/* Move Ordering Functions for Alpha-Beta Optimization */

/* Initialize move ordering system */
static void init_move_ordering(void)
{
    memset(&move_ordering, 0, sizeof(move_ordering));
}

/* Clear move ordering statistics */
static void clear_move_ordering_stats(void)
{
    memset(&move_ordering.stats, 0, sizeof(move_ordering.stats));
}

/* Update history heuristic when a move causes cutoff */
static void update_history(int depth, int col, int rot, int bonus)
{
    if (depth < 0 || depth >= MAX_SEARCH_DEPTH || col < 0 ||
        col >= GRID_WIDTH || rot < 0 || rot >= 4)
        return;

    move_ordering.history_table[depth][col][rot] += bonus;

    /* Prevent overflow and maintain relative differences */
    if (move_ordering.history_table[depth][col][rot] <= 10000)
        return;

    /* Age all history values */
    for (int d = 0; d < MAX_SEARCH_DEPTH; d++) {
        for (int c = 0; c < GRID_WIDTH; c++) {
            for (int r = 0; r < 4; r++) {
                move_ordering.history_table[d][c][r] /= 2;
            }
        }
    }
}

/* Get history heuristic score for a move */
static int get_history_score(int depth, int col, int rot)
{
    if (depth >= 0 && depth < MAX_SEARCH_DEPTH && col >= 0 &&
        col < GRID_WIDTH && rot >= 0 && rot < 4) {
        return move_ordering.history_table[depth][col][rot];
    }
    return 0;
}

/* Update killer moves when a move causes cutoff */
static void update_killers(int depth, int col, int rot)
{
    if (depth >= 0 && depth < MAX_SEARCH_DEPTH) {
        /* Check if this move is already a killer */
        for (int i = 0; i < KILLER_SLOTS; i++) {
            if (move_ordering.killer_moves[depth][i].valid &&
                move_ordering.killer_moves[depth][i].col == col &&
                move_ordering.killer_moves[depth][i].rot == rot) {
                return; /* Already a killer, no need to update */
            }
        }

        /* Shift killers and add new one */
        for (int i = KILLER_SLOTS - 1; i > 0; i--) {
            move_ordering.killer_moves[depth][i] =
                move_ordering.killer_moves[depth][i - 1];
        }

        move_ordering.killer_moves[depth][0].col = col;
        move_ordering.killer_moves[depth][0].rot = rot;
        move_ordering.killer_moves[depth][0].valid = true;
    }
}

/* Check if a move is a killer move */
static bool is_killer_move(int depth, int col, int rot)
{
    if (depth >= 0 && depth < MAX_SEARCH_DEPTH) {
        for (int i = 0; i < KILLER_SLOTS; i++) {
            if (move_ordering.killer_moves[depth][i].valid &&
                move_ordering.killer_moves[depth][i].col == col &&
                move_ordering.killer_moves[depth][i].rot == rot) {
                return true;
            }
        }
    }
    return false;
}

/* Check if a move is from principal variation */
static bool is_pv_move(int ply, int col, int rot)
{
    if (move_ordering.principal_variation.valid &&
        ply * 2 + 1 < move_ordering.principal_variation.length) {
        return (move_ordering.principal_variation.moves[ply * 2] == col &&
                move_ordering.principal_variation.moves[ply * 2 + 1] == rot);
    }
    return false;
}

/* Calculate move ordering score for sorting */
static int calculate_move_score(int depth, int ply, int col, int rot)
{
    int score = 0;

    /* Principal variation gets highest priority */
    if (is_pv_move(ply, col, rot))
        score += 1000000; /* Highest priority */

    /* Killer moves get high priority */
    if (is_killer_move(depth, col, rot))
        score += 100000; /* Second highest */

    /* History heuristic provides learned priority */
    score += get_history_score(depth, col, rot);

    /* Center-column preference as fallback */
    int center = GRID_WIDTH / 2;
    int distance_from_center = abs(col - center);
    score += 1000 - (distance_from_center * 100); /* Prefer center columns */

    return score;
}

/* Advanced move ordering using multiple heuristics */
struct move_candidate {
    int col, rot;
    int score;
};

static int compare_moves(const void *a, const void *b)
{
    const struct move_candidate *move_a = a;
    const struct move_candidate *move_b = b;
    return move_b->score - move_a->score; /* Descending order */
}

static void order_moves_advanced(struct move_candidate *moves,
                                 int count,
                                 int depth,
                                 int ply)
{
    /* Calculate scores for all moves */
    for (int i = 0; i < count; i++) {
        moves[i].score =
            calculate_move_score(depth, ply, moves[i].col, moves[i].rot);
    }

    /* Sort by score (highest first) */
    qsort(moves, count, sizeof(struct move_candidate), compare_moves);
}

/* Shape-aware hole penalty calculation
 *
 * When falling_block is provided, skips counting holes that would be
 * filled by the current falling piece, providing more accurate evaluation.
 */
/* Unified hole penalty calculation with proper caching */
static float get_hole_penalty_with_block(const grid_t *g,
                                         const block_t *falling_block)
{
    if (!g || !g->relief)
        return 0.0f;

    /* For cached version without falling block, use hash-based caching */
    if (!falling_block) {
        uint32_t idx = g->hash & METRICS_CACHE_MASK;
        struct metrics_entry *entry = &metrics_cache[idx];

        /* Cache hit - return immediately */
        if (entry->grid_key == g->hash)
            return entry->hole_penalty;
    }

    /* Cache miss or with falling block - compute penalty */
    float total_penalty = 0.0f;

    for (int x = 0; x < g->width; x++) {
        int top = g->relief[x];
        if (top < 0 || g->gaps[x] == 0)
            continue;

        for (int y = top - 1; y >= 0; y--) {
            if (!move_cell_occupied(g, x, y)) {
                /* If falling block provided, check if it fills this hole */
                if (falling_block && is_block_coordinate(falling_block, x, y))
                    continue; /* Skip holes filled by falling block */

                int depth = top - y;
                /* Single penalty calculation with depth weighting */
                total_penalty +=
                    HOLE_PENALTY * (1.0f + HOLE_DEPTH_WEIGHT * depth);
            }
        }
    }

    /* Update cache entry only for grid-only calculations */
    if (!falling_block) {
        uint32_t idx = g->hash & METRICS_CACHE_MASK;
        struct metrics_entry *entry = &metrics_cache[idx];
        entry->grid_key = g->hash;
        entry->hole_penalty = total_penalty;
    }

    return total_penalty;
}

/* Fast cached hole penalty with early exit on cache hit */
static float get_hole_penalty(const grid_t *g)
{
    return get_hole_penalty_with_block(g, NULL);
}

/* Fast cached bumpiness with early exit */
static int get_bumpiness(const grid_t *g)
{
    if (!g || !g->relief)
        return 0;

    uint32_t idx = g->hash & METRICS_CACHE_MASK;
    struct metrics_entry *entry = &metrics_cache[idx];

    /* Cache hit - return immediately */
    if (entry->grid_key == g->hash)
        return entry->bumpiness;

    /* Cache miss - compute and store */
    int bump = 0, last_height = -1;
    for (int i = 0; i < g->width; i++) {
        int height = g->relief[i] + 1;
        if (last_height >= 0)
            bump += abs(height - last_height);
        last_height = height;
    }

    /* Update cache entry */
    entry->grid_key = g->hash;
    entry->bumpiness = (uint16_t) MIN(bump, 65535);

    return bump;
}

/* Fast cached transitions with early exit */
static void get_transitions(const grid_t *g, int *row_out, int *col_out)
{
    if (!g) {
        if (row_out)
            *row_out = 0;
        if (col_out)
            *col_out = 0;
        return;
    }

    uint32_t idx = g->hash & METRICS_CACHE_MASK;
    struct metrics_entry *entry = &metrics_cache[idx];

    /* Cache hit - return immediately */
    if (entry->grid_key == g->hash) {
        if (row_out)
            *row_out = entry->row_trans;
        if (col_out)
            *col_out = entry->col_trans;
        return;
    }

    /* Cache miss - compute and store */
    int row_t = 0, col_t = 0;
    uint16_t prev_mask = 0xFFFF;
    const int w = g->width;

    for (int y = g->height - 1; y >= 0; y--) {
        uint16_t mask;
        if (g->rows[y] == 0) {
            /* Empty row optimization */
            mask = 0;
        } else if (g->rows[y] == g->full_mask) {
            /* Full row optimization */
            mask = (uint16_t) ((1u << w) - 1);
        } else {
            /* Partial row - extract directly from packed representation */
            mask = (uint16_t) (g->rows[y] & ((1u << w) - 1));
        }

        /* Row transitions */
        int first = mask & 1u;
        int last = (mask >> (w - 1)) & 1u;
        row_t += (1 ^ first) + (1 ^ last);

        uint16_t diff = mask ^ (mask >> 1);
        diff &= (uint16_t) ((1u << (w - 1)) - 1);
        row_t += POPCOUNT(diff);

        /* Column transitions */
        col_t += POPCOUNT(prev_mask ^ mask);
        prev_mask = mask;
    }

    /* Update cache entry */
    entry->grid_key = g->hash;
    entry->row_trans = (uint16_t) MIN(row_t, 65535);
    entry->col_trans = (uint16_t) MIN(col_t, 65535);

    if (row_out)
        *row_out = row_t;
    if (col_out)
        *col_out = col_t;
}

/* Fast cached well depth with early exit */
static int get_well_depth(const grid_t *g)
{
    if (!g || !g->relief)
        return 0;

    uint32_t idx = g->hash & METRICS_CACHE_MASK;
    struct metrics_entry *entry = &metrics_cache[idx];

    /* Cache hit - return immediately */
    if (entry->grid_key == g->hash)
        return entry->well_depth;

    /* Cache miss - compute and store */
    int depth = 0;
    for (int x = 0; x < g->width; x++) {
        int left = (x == 0) ? g->height : (g->relief[x - 1] + 1);
        int right = (x == g->width - 1) ? g->height : (g->relief[x + 1] + 1);
        int height = g->relief[x] + 1;
        if (left > height && right > height)
            depth += MIN(left, right) - height;
    }

    /* Update cache entry */
    entry->grid_key = g->hash;
    entry->well_depth = (uint16_t) MIN(depth, 65535);

    return depth;
}

/* Fast cached crevice detection with early exit
 *
 * Detects narrow gaps (crevices) that are difficult to fill.
 * These are columns significantly lower than both neighbors.
 *
 * A problematic crevice forms when:
 * - Column is significantly lower than both neighbors
 * - Gap is narrow (1-2 columns wide)
 * - Contains holes that make filling more difficult
 */
static int get_crevices_with_block(const grid_t *g,
                                   const block_t *falling_block)
{
    if (!g || !g->relief)
        return 0;

    /* For cached version without falling block, use hash-based caching */
    if (!falling_block) {
        uint32_t idx = g->hash & METRICS_CACHE_MASK;
        struct metrics_entry *entry = &metrics_cache[idx];

        /* Cache hit - return immediately */
        if (entry->grid_key == g->hash)
            return entry->crevices;
    }

    /* Cache miss or with falling block - compute and store if cacheable */
    float crevice_penalty = 0.0f;

    /* Refined crevice detection: distinguish between manageable gaps
     * and truly problematic narrow, deep crevices.
     * When falling_block is provided, skip cells it occupies.
     */
    for (int x = 1; x < g->width - 1; x++) {
        int left_height = g->relief[x - 1] + 1;
        int curr_height = g->relief[x] + 1;
        int right_height = g->relief[x + 1] + 1;

        /* Check if this is a crevice (column lower than both neighbors) */
        if (curr_height < left_height - 1 && curr_height < right_height - 1) {
            /* Calculate the depth of the crevice */
            int min_neighbor =
                (left_height < right_height) ? left_height : right_height;
            int depth = min_neighbor - curr_height;

            float penalty = 0.0f;

            /* Check if the crevice is 1-wide (most severe case)
             * A 1-wide crevice is when both immediate neighbors are higher
             */
            bool is_one_wide = true;

            /* Check if there is potential for the crevice to be wider */
            if (x > 1 && g->relief[x - 2] + 1 <= curr_height)
                is_one_wide = false; /* Left side has room */
            if (x < g->width - 2 && g->relief[x + 2] + 1 <= curr_height)
                is_one_wide = false; /* Right side has room */

            if (is_one_wide) {
                /* 1-wide crevices are very problematic, especially when deep */
                penalty = depth * 1.5f;
            } else {
                /* Wider crevices are less severe as they're easier to fill */
                penalty = depth * 0.5f;
            }

            /* Add extra penalty for any holes inside the crevice
             * Holes in crevices are particularly bad as they're hard to clear
             */
            if (g->gaps[x] > 0)
                penalty += g->gaps[x] * 1.2f;

            /* If falling block provided, check if it helps fill this crevice */
            if (falling_block) {
                bool block_helps = false;
                for (int y = curr_height; y < min_neighbor; y++) {
                    if (is_block_coordinate(falling_block, x, y)) {
                        block_helps = true;
                        break;
                    }
                }
                /* Reduce penalty if falling block helps fill the crevice */
                if (block_helps)
                    penalty *= 0.3f; /* Reduce penalty by 70% */
            }

            crevice_penalty += penalty;
        }
    }

    /* Convert float penalty to integer for compatibility */
    /* Round to nearest int */
    int crevice_count = (int) (crevice_penalty + 0.5f);

    /* Update cache entry only for grid-only calculations */
    if (!falling_block) {
        uint32_t idx = g->hash & METRICS_CACHE_MASK;
        struct metrics_entry *entry = &metrics_cache[idx];
        entry->grid_key = g->hash;
        entry->crevices = (uint16_t) MIN(crevice_count, 65535);
    }

    return crevice_count;
}

static int get_crevices(const grid_t *g)
{
    return get_crevices_with_block(g, NULL);
}

/* Fast cached overhang detection with early exit
 *
 * Detects overhangs: blocks extending over empty spaces that create
 * difficult-to-fill pockets. An overhang occurs when a filled cell has
 * an empty cell directly below it, but is supported by filled cells
 * on both bottom-left and bottom-right diagonals.
 */
static int get_overhangs(const grid_t *g)
{
    if (!g)
        return 0;

    uint32_t idx = g->hash & METRICS_CACHE_MASK;
    struct metrics_entry *entry = &metrics_cache[idx];

    /* Cache hit - return immediately */
    if (entry->grid_key == g->hash)
        return entry->overhangs;

    /* Cache miss - compute and store */
    int overhang_count = 0;
    for (int x = 0; x < g->width; x++) {
        for (int y = 1; y < g->height; y++) { /* Start from y=1 to check y-1 */
            if (move_cell_occupied(g, x, y) &&
                !move_cell_occupied(g, x, y - 1)) {
                /* This cell is an overhang if the cell below it is empty, AND
                 * its immediate left-bottom and right-bottom neighbors are
                 * filled
                 */
                bool left_bottom_filled =
                    (x == 0) ? true : move_cell_occupied(g, x - 1, y - 1);
                bool right_bottom_filled =
                    (x == g->width - 1) ? true
                                        : move_cell_occupied(g, x + 1, y - 1);

                if (left_bottom_filled && right_bottom_filled)
                    overhang_count++;
            }
        }
    }

    /* Update cache entry */
    entry->grid_key = g->hash;
    entry->overhangs = (uint16_t) MIN(overhang_count, 65535);

    return overhang_count;
}

/* AI evaluation function: Multi-heuristic position assessment
 *
 * This is the core AI evaluation function that assesses Tetris board positions
 * using a weighted combination of strategic heuristics. The algorithm combines
 * evolved weights (trained through genetic algorithms) with hand-tuned
 * heuristics to produce a single score representing position quality.
 *
 * Evaluation pipeline:
 *
 * 1. Termination check: Fast top-out detection for immediate losing positions
 *    - Scans relief array for pieces reaching the ceiling
 *    - Returns large negative penalty to avoid game-ending moves
 *
 * 2. Cache lookup: Multi-level caching system for performance
 *    - Combines Zobrist grid hash with weights hash for cache key
 *    - ~95% cache hit rate in typical gameplay reduces computation
 *    - Cache size tuned for L2/L3 cache residence
 *
 * 3. Feature extraction: Mathematical analysis of board structure
 *    - Height statistics (max, average, variance) measure stack distribution
 *    - Gap counting identifies holes that are difficult to fill
 *    - Discontinuity measurement penalizes uneven surfaces
 *    - Crevice detection finds narrow gaps creating future problems
 *
 * 4. Weighted scoring: Linear combination with evolved coefficients
 *    - Each feature multiplied by genetically optimized weight
 *    - Weights balance conflicting objectives (height vs. stability)
 *    - Current weights evolved over 500+ generations with fitness=1269
 *
 * 5. Structural penalties: Hand-tuned heuristics for board quality
 *    - Hole penalty: Exponentially penalizes buried empty cells
 *      Formula: base_penalty * holes + depth_weight * total_depth
 *    - Bumpiness: Surface roughness that complicates piece placement
 *    - Well depth: Deep columns that can trap pieces
 *    - Transitions: Dellacherie heuristic counting filled/empty boundaries
 *
 * 6. Height management: Balance between safety and line-clearing setup
 *    - General height penalty discourages dangerous high stacks
 *    - Tall stack bonus rewards controlled building for Tetris opportunities
 *    - Non-linear scaling prevents excessive risk-taking
 *
 * Performance optimizations:
 * - Metrics caching: Expensive computations cached by grid hash
 * - Feature vectorization: SIMD-friendly linear algebra
 * - Early termination: Fail-fast for obviously poor positions
 * - Cache-friendly memory layout: Hot data fits in processor cache
 *
 * Score interpretation:
 * - Positive scores indicate favorable positions
 * - Negative scores suggest problematic structures
 * - Score differences of 0.1-1.0 represent meaningful position quality gaps
 * - Large negative scores (-1000+) indicate terminal or near-terminal states
 */

/* Count complete rows in a height range for Tetris potential analysis */
static int count_complete_rows(const grid_t *g, int min_row, int max_row)
{
    if (!g)
        return 0;

    int complete_rows = 0;
    int almost_complete = 0;

    /* Clamp range to valid bounds */
    min_row = MAX(0, min_row);
    max_row = MIN(g->height - 1, max_row);

    for (int y = min_row; y <= max_row; y++) {
        /* Use bit counting for efficiency */
        uint64_t row_bits = g->rows[y] & g->full_mask;
        int filled_cells = POPCOUNT(row_bits);

        if (filled_cells == g->width) {
            complete_rows++;
        } else if (filled_cells == g->width - 1) {
            /* Row needs just one block - potential for Tetris */
            almost_complete++;
        }
    }

    /* Return weighted score: complete rows plus partial credit for almost
     * complete */
    return complete_rows * 2 + almost_complete;
}

/* Evaluate the board's potential for achieving a Tetris */
static float evaluate_tetris_potential(const grid_t *g)
{
    if (!g || !g->relief)
        return 0.0f;

    float tetris_score = 0.0f;
    int well_col = -1;
    bool has_well = grid_is_tetris_ready(g, &well_col);

    if (has_well) {
        /* Strong bonus for having a clean well */
        tetris_score += TETRIS_WELL_BONUS;

        /* Check well depth and quality */
        float well_quality_multiplier = 1.0f;
        int well_depth = g->height - g->relief[well_col] - 1;
        if (well_depth >= PERFECT_WELL_DEPTH) {
            /* Perfect depth for Tetris */
            tetris_score += TETRIS_READY_BONUS;
            well_quality_multiplier = 1.5f; /* Higher quality well */

            /* Check how many rows are ready to clear */
            int ready_rows =
                count_complete_rows(g, g->relief[well_col],
                                    g->relief[well_col] + PERFECT_WELL_DEPTH);

            if (ready_rows >= 3) {
                /* Almost ready for Tetris! */
                tetris_score += TETRIS_BONUS * 0.5f * well_quality_multiplier;
            }
        }

        /* Penalty if well is too shallow */
        if (well_depth < 2) {
            tetris_score -= 2.0f;
        }
    } else {
        /* Look for potential well locations */
        for (int x = 0; x < g->width; x++) {
            bool could_be_well = true;

            /* Check if column could become a well */
            if (x == 0) {
                /* Leftmost column */
                if (g->relief[1] < g->relief[0] + 2)
                    could_be_well = false;
            } else if (x == g->width - 1) {
                /* Rightmost column */
                if (g->relief[g->width - 2] < g->relief[x] + 2)
                    could_be_well = false;
            } else {
                /* Middle columns */
                if (g->relief[x - 1] < g->relief[x] + 2 ||
                    g->relief[x + 1] < g->relief[x] + 2)
                    could_be_well = false;
            }

            if (could_be_well && g->gaps[x] == 0) {
                /* Potential well location with no holes */
                tetris_score += 0.5f;
            }
        }
    }

    return tetris_score;
}

/* Dynamic crisis assessment based on multiple board conditions
 *
 * Analyzes board state comprehensively to determine crisis level:
 * - Height pressure: Stack approaching danger zone
 * - Structural problems: Holes and surface irregularities
 * - Adaptive scaling: Returns multiplier from 1.0 (stable) upwards
 *
 * This enables context-aware strategy switching:
 * - Clean boards: Focus on setup and optimization
 * - Crisis conditions: Shift to survival and damage control
 */
static float get_crisis_level(const grid_t *g, const float *features)
{
    if (!g || !features)
        return 1.0f;

    float height_ratio = features[FEATIDX_RELIEF_MAX] / (float) g->height;
    float holes = features[FEATIDX_GAPS];

    /* Multi-level crisis model */
    if (height_ratio > 0.9f || holes > 15)
        return 5.0f; /* Panic */
    if (height_ratio > 0.85f || holes > 12)
        return 4.0f; /* Desperate */
    if (height_ratio > 0.8f || holes > 10)
        return 3.0f; /* Emergency */
    if (height_ratio > 0.7f || holes > 8)
        return 2.5f; /* High crisis */
    if (height_ratio > 0.6f || holes > 6)
        return 2.0f; /* Crisis */

    /* Check for chaotic handoff situation */
    if (is_chaotic_handoff(g))
        return 2.8f; /* Boost for chaotic patterns */

    return 1.0f; /* Normal mode */
}

/* Detect if this is a chaotic handoff situation from human play
 * Returns true if board shows signs of random/poor human placement
 * that requires special recovery strategies
 */
static bool is_chaotic_handoff(const grid_t *g)
{
    if (!g || !g->relief)
        return false;

    int chaos_indicators = 0;
    int total_holes = 0;
    int max_height = 0;
    int min_height = g->height;
    int isolated_blocks = 0;

    /* Calculate basic metrics */
    for (int x = 0; x < g->width; x++) {
        int height = g->relief[x] + 1;
        total_holes += g->gaps[x];
        if (height > max_height)
            max_height = height;
        if (height < min_height)
            min_height = height;
    }

    /* Check for chaos indicators */

    /* 1. Excessive holes relative to height */
    if (total_holes > 8 && max_height < 12)
        chaos_indicators++;

    /* 2. Extreme height variance (choppy surface) */
    int height_variance = max_height - min_height;
    if (height_variance > 8)
        chaos_indicators++;

    /* 3. Many isolated single blocks */
    for (int x = 1; x < g->width - 1; x++) {
        int left_h = g->relief[x - 1] + 1;
        int center_h = g->relief[x] + 1;
        int right_h = g->relief[x + 1] + 1;

        /* Block is isolated if much higher than both neighbors */
        if (center_h > left_h + 2 && center_h > right_h + 2)
            isolated_blocks++;
    }

    if (isolated_blocks > 2)
        chaos_indicators++;

    /* 4. High hole density in low areas */
    if (total_holes > 6 && max_height < 10)
        chaos_indicators++;

    /* Consider chaotic if multiple indicators present */
    return chaos_indicators >= 2;
}

/* Applies all structural penalties, scaled by crisis level and setup discount
 */
static float apply_structural_penalties(const grid_t *g,
                                        float crisis_multiplier,
                                        float setup_discount)
{
    float penalty = 0.0f;

    /* Hole penalties, scaled by crisis level and setup discount */
    penalty += get_hole_penalty(g) * crisis_multiplier * setup_discount;

    /* Surface structure penalties, scaled by setup discount only */
    penalty += BUMPINESS_PENALTY * get_bumpiness(g) * setup_discount;
    penalty += WELL_PENALTY * get_well_depth(g) * setup_discount;
    penalty += CREVICE_PENALTY * get_crevices(g) * setup_discount;
    penalty += OVERHANG_PENALTY * get_overhangs(g) * setup_discount;

    /* Transition penalties (Dellacherie heuristic for boundary analysis) */
    int row_trans, col_trans;
    get_transitions(g, &row_trans, &col_trans);
    penalty += ROW_TRANS_PENALTY * row_trans; /* Horizontal boundaries */
    penalty += COL_TRANS_PENALTY * col_trans; /* Vertical boundaries */

    return penalty;
}

/* Core evaluation function with optional piece-aware caching */
static float eval_grid(const grid_t *g,
                       const float *weights,
                       const shape_t *shape,
                       int rotation,
                       int column)
{
    if (!g || !weights)
        return WORST_SCORE;

    /* Fast top-out detection: immediate losing condition */
    for (int x = 0; x < g->width; x++) {
        if (g->relief[x] >= g->height - 1)
            return -TOPOUT_PENALTY;
    }

    /* Enhanced cache key: grid + weights + piece context */
    uint64_t combined_key = g->hash ^ hash_weights(weights);
    if (shape) {
        /* Include piece context for more precise caching */
        combined_key ^= hash_piece_context(shape, rotation, column);
    }
    combined_key *= 0x2545F4914F6CDD1DULL; /* Avalanche hash mixing */

    struct eval_cache_entry *entry =
        &eval_cache[combined_key & (EVAL_CACHE_SIZE - 1)];
    if (entry->key == combined_key)
        return entry->val; /* Enhanced evaluation cache hit */

    /* Cache miss - compute using fast cached metrics */
    float features[N_FEATIDX];
    calc_features(g, features, NULL);

    /* Phase 1: Weighted linear combination of evolved features */
    float score = 0.0f;
    for (int i = 0; i < N_FEATIDX; i++)
        score += features[i] * weights[i];

    /* Phase 2: Apply unified structural penalties with crisis scaling */
    float crisis_multiplier = get_crisis_level(g, features);

    /* Unified penalty application - avoid double counting */
    int well_col = -1;
    bool tetris_ready = grid_is_tetris_ready(g, &well_col);
    /* Scale down penalties if building for good Tetris or if not in crisis. */
    float setup_discount = 1.0f;
    if (crisis_multiplier < 1.5f) {
        /* In non-crisis situations, be lenient with structural penalties */
        setup_discount = tetris_ready ? 0.7f : 0.85f;
    }

    /* Apply all structural penalties through dedicated helper function */
    score -= apply_structural_penalties(g, crisis_multiplier, setup_discount);

    /* Phase 3: Unified height management system */
    int total_height = (int) (features[FEATIDX_RELIEF_AVG] * g->width);
    int max_height = (int) features[FEATIDX_RELIEF_MAX];

    /* Adjusted height penalty */
    float height_penalty_factor = HEIGHT_PENALTY;

    /* Reduce height penalty when building for Tetris */
    if (max_height >= STRATEGIC_HEIGHT_START &&
        max_height <= TETRIS_SETUP_HEIGHT) {
        int well_col = -1;
        if (grid_is_tetris_ready(g, &well_col)) {
            /* Significantly reduce height penalty when Tetris-ready */
            height_penalty_factor *= 0.5f;
        }
    }

    score -= height_penalty_factor * total_height * crisis_multiplier;

    /* Add Tetris potential evaluation */
    float tetris_potential = evaluate_tetris_potential(g);
    /* Less weight in crisis */
    score += tetris_potential * (2.0f - crisis_multiplier);

    /* Strategic height bonus for Tetris setups - enhanced in appropriate
     * conditions */
    if (max_height >= STRATEGIC_HEIGHT_START && crisis_multiplier < 1.5f) {
        int capped_height = MIN(max_height, STRATEGIC_HEIGHT_CAP);
        float height_bonus = (capped_height - STRATEGIC_HEIGHT_START + 1) *
                             STRATEGIC_HEIGHT_BONUS;

        /* Check for Tetris-ready conditions */
        int well_col = -1;
        bool tetris_ready = grid_is_tetris_ready(g, &well_col);

        if (tetris_ready) {
            /* Significant bonus for maintaining Tetris-ready board */
            height_bonus += TETRIS_READY_BONUS;

            /* Extra bonus if height is ideal for Tetris */
            if (max_height >= TETRIS_BUILD_HEIGHT &&
                max_height <= TETRIS_SETUP_HEIGHT) {
                height_bonus += TETRIS_WELL_BONUS;
            }
        }

        /* Only reduce bonus significantly in high crisis */
        if (crisis_multiplier > 1.3f)
            height_bonus /= (crisis_multiplier * crisis_multiplier);
        score += height_bonus;
    }

    /* In crisis, prioritize any line clear for survival */
    if (crisis_multiplier > 1.5f && g->n_last_cleared > 0) {
        score += EMERGENCY_CLEAR_BONUS * g->n_last_cleared * crisis_multiplier;

        /* Add a bonus for "clean" clears that don't create new holes */
        float current_holes = features[FEATIDX_GAPS];
        float hole_change = current_holes - (float) (g->n_last_cleared * 4);

        /* Bonus if holes didn't increase relative to expected reduction */
        if (hole_change <= current_holes * 0.1f)
            score += 2.0f * crisis_multiplier; /* Clean clear bonus */
    }

    /* Bonus for recovering from a chaotic handoff */
    if (is_chaotic_handoff(g) &&
        depth_stats.piece_count < INITIAL_ASSESSMENT_PIECES)
        score += HANDOFF_RECOVERY_BONUS * crisis_multiplier;

    /* Advanced crisis recovery strategies with pattern recognition */
    if (crisis_multiplier > 1.2f) {
        /* Strategy 1: Height reduction bonus */
        if (max_height < g->height - 2) {
            float height_buffer = (float) (g->height - max_height);
            float height_recovery = height_buffer * 0.25f * crisis_multiplier;
            score += height_recovery;
        }

        /* Strategy 2: Hole management bonus */
        float holes = features[FEATIDX_GAPS];
        if (holes < 8) {
            float hole_bonus = (8 - holes) * 0.3f * crisis_multiplier;
            score += hole_bonus;
        }

        /* Strategy 3: Surface smoothness bonus for easier recovery */
        float variance = features[FEATIDX_RELIEF_VAR];
        float avg = features[FEATIDX_RELIEF_AVG];
        if (avg > 0 && variance / (avg * avg) < 0.15f)
            score += SURVIVAL_BONUS * crisis_multiplier;

        /* Strategy 4: Simple accessibility check (faster) */
        if (features[FEATIDX_DISCONT] < g->width * 2)
            score += 0.3f * crisis_multiplier;

        /* Strategy 5: Simple valley bonus - check only around placement */
        if (shape && column >= 1 && column < g->width - 1) {
            int left_height = g->relief[column - 1] + 1;
            int center_height = g->relief[column] + 1;
            int right_height = g->relief[column + 1] + 1;

            /* Quick valley check without division */
            if ((left_height - center_height) >= VALLEY_DEPTH_THRESHOLD &&
                (right_height - center_height) >= VALLEY_DEPTH_THRESHOLD) {
                score += VALLEY_FILL_BONUS * crisis_multiplier;
            }
        }

        /* Emergency mode tactics */
        if (crisis_multiplier > 2.0f) {
            /* In emergency, ANY line clear is valuable */
            if (g->n_last_cleared > 0)
                score += EMERGENCY_CLEAR_BONUS * g->n_last_cleared;

            /* Reward pattern matching for crisis recovery */
            if (shape) {
                /* Check if this move helps resolve crisis patterns */
                bool helps_crisis = false;

                /* Check if piece placement reduces height variance */
                if (variance > SURFACE_CHAOS_THRESHOLD && column >= 0)
                    helps_crisis = true;

                /* Check if piece helps clear critical holes */
                if (holes > CRITICAL_HOLE_THRESHOLD)
                    helps_crisis = true;

                if (helps_crisis) {
                    /* Crisis pattern bonus */
                    score += 1.5f * crisis_multiplier;
                }
            }
        }
    }

    /* In extreme crisis, override all other heuristics and focus exclusively
     * on survival.
     */
    if (max_height >= PANIC_MODE_THRESHOLD) {
        float panic_score = 0.0f;

        /* Priority 1: Massive bonus for any line clear. */
        if (g->n_last_cleared > 0)
            panic_score += 100.0f * g->n_last_cleared;

        /* Priority 2: Heavy penalty for increasing height. */
        panic_score -= max_height * 10.0f;

        /* Priority 3: Penalty for holes. */
        panic_score -= features[FEATIDX_GAPS] * 20.0f;

        /* Use panic score exclusively, ignoring other factors. */
        score = panic_score;
    }

    /* Simplified surface quality for speed */
    float surface_quality = 0.0f;

    /* Quick surface scan */
    for (int x = 0; x < g->width - 1; x++) {
        int height_diff = abs(g->relief[x] - g->relief[x + 1]);
        if (height_diff <= 1) {
            surface_quality += 0.1f;
        } else if (height_diff > 3) {
            surface_quality -= (crisis_multiplier > 1.5f) ? 0.12f : 0.05f;
        }
    }

    /* Simple crisis bonus for smooth surfaces */
    if (crisis_multiplier > 1.5f && surface_quality > 0.5f)
        surface_quality += 0.5f;

    score += surface_quality;

    /* Note: Overhang penalty now included in apply_structural_penalties() */

    /* Store computed result in evaluation cache */
    entry->key = combined_key;
    entry->val = score;

    return score;
}


/* Shape-aware evaluation for more accurate scoring during placement testing
 *
 * When falling_block is provided, uses shape-aware metrics that account
 * for cells the falling piece would fill, providing more accurate evaluation
 * of the resulting position.
 */

/* Simplified shallow evaluation - just use main evaluation */
static inline float eval_shallow_with_context(const grid_t *g,
                                              const float *weights,
                                              const shape_t *shape,
                                              int rotation,
                                              int column)
{
    return eval_grid(g, weights, shape, rotation, column);
}


/* Determine adaptive beam size based on board state and crisis patterns */
static int calc_beam_size(const grid_t *g)
{
    if (!g || !g->relief)
        return BEAM_SIZE;

    /* Single pass to get height and holes for crisis assessment */
    int max_height = 0;
    int hole_count = 0;
    for (int x = 0; x < g->width; x++) {
        int height = g->relief[x] + 1;
        if (height > max_height)
            max_height = height;
        hole_count += g->gaps[x];
    }

    /* Use height and holes for beam sizing */
    beam_stats.adaptive_expansions++;

    if (max_height >= PANIC_MODE_THRESHOLD || hole_count > 15) {
        return BEAM_SIZE_MAX + 4; /* Panic mode */
    } else if (max_height >= DESPERATE_MODE_THRESHOLD || hole_count > 12) {
        return BEAM_SIZE_MAX + 2; /* Desperate mode */
    } else if (max_height >= EMERGENCY_MODE_THRESHOLD || hole_count > 10) {
        return BEAM_SIZE_MAX; /* Emergency mode */
    } else if (max_height >= CRISIS_MODE_THRESHOLD ||
               hole_count > CRITICAL_HOLE_THRESHOLD) {
        return BEAM_SIZE + 2; /* Crisis mode */
    }

    return BEAM_SIZE;
}

/* Clear evaluation caches */
static void clear_eval_cache(void)
{
    memset(eval_cache, 0, sizeof(eval_cache));
    memset(metrics_cache, 0, sizeof(metrics_cache));
    weights_cache.valid = false;
}

/* Initialize move cache */
static bool cache_init_minimal(int max_depth, const grid_t *template_grid)
{
    if (move_cache.initialized)
        return true;

    /* Reduce depth to minimum workable level */
    int reduced_depth = (max_depth < 4) ? max_depth : 4;
    move_cache.size = reduced_depth;

    /* Allocate minimal structures */
    move_cache.search_blocks = nalloc(reduced_depth * sizeof(block_t), NULL);
    move_cache.cand_moves = nalloc(reduced_depth * sizeof(move_t), NULL);

    if (!move_cache.search_blocks || !move_cache.cand_moves) {
        cache_cleanup();
        return false;
    }

    /* Use minimal working grid without stacks */
    grid_t *working = &move_cache.working_grid;
    *working = *template_grid; /* rows array is copied inline */

    /* Allocate only essential structures */
    working->relief =
        ncalloc(template_grid->width, sizeof(*working->relief), NULL);
    working->gaps = ncalloc(template_grid->width, sizeof(*working->gaps), NULL);
    working->stack_cnt =
        ncalloc(template_grid->width, sizeof(*working->stack_cnt), NULL);
    working->full_rows =
        ncalloc(template_grid->height, sizeof(*working->full_rows), NULL);

    if (!working->relief || !working->gaps || !working->stack_cnt ||
        !working->full_rows) {
        cache_cleanup();
        return false;
    }

    /* Skip per-column stacks to save memory */
    working->stacks = NULL;

    move_cache.initialized = true;
    return true;
}

static bool cache_init(int max_depth, const grid_t *template_grid)
{
    if (move_cache.initialized)
        return true;

    move_cache.size = max_depth;


    /* Allocate single working grid and other structures */
    move_cache.search_blocks = nalloc(max_depth * sizeof(block_t), NULL);
    move_cache.cand_moves = nalloc(max_depth * sizeof(move_t), NULL);

    if (!move_cache.search_blocks || !move_cache.cand_moves) {
        cache_cleanup();
        /* Fallback to minimal cache */
        return cache_init_minimal(max_depth, template_grid);
    }

    /* Initialize the single working grid - this replaces multiple eval_grids */
    grid_t *working = &move_cache.working_grid;
    *working = *template_grid; /* rows array is copied inline */

    /* Allocate other working grid arrays */
    working->stacks =
        ncalloc(template_grid->width, sizeof(*working->stacks), NULL);
    working->relief =
        ncalloc(template_grid->width, sizeof(*working->relief), NULL);
    working->gaps = ncalloc(template_grid->width, sizeof(*working->gaps), NULL);
    working->stack_cnt =
        ncalloc(template_grid->width, sizeof(*working->stack_cnt), NULL);
    working->full_rows =
        ncalloc(template_grid->height, sizeof(*working->full_rows), NULL);

    if (!working->stacks || !working->relief || !working->gaps ||
        !working->stack_cnt || !working->full_rows) {
        cache_cleanup();
        /* Fallback to minimal cache */
        return cache_init_minimal(max_depth, template_grid);
    }

    for (int c = 0; c < template_grid->width; c++) {
        working->stacks[c] =
            ncalloc(template_grid->height, sizeof(*working->stacks[c]),
                    working->stacks);
        if (!working->stacks[c]) {
            cache_cleanup();
            /* Fallback to minimal cache */
            return cache_init_minimal(max_depth, template_grid);
        }
    }

    move_cache.initialized = true;
    return true;
}

/* Clear beam search statistics */
static inline void clear_beam_stats(void)
{
    memset(&beam_stats, 0, sizeof(beam_stats));
}

/* Clear filter performance statistics */
static inline void clear_filter_stats(void)
{
    memset(&filter_stats, 0, sizeof(filter_stats));
}

/* Update filter efficiency calculation */
static inline void update_filter_efficiency(void)
{
    if (filter_stats.total_candidates > 0) {
        int filtered = filter_stats.total_candidates - filter_stats.evaluated;
        filter_stats.filter_efficiency =
            (100.0f * filtered) / filter_stats.total_candidates;
    }
}

/* Clear search performance statistics */
static inline void clear_depth_stats(void)
{
    memset(&depth_stats, 0, sizeof(depth_stats));
}

/* Cleanup move cache */
static void cache_cleanup(void)
{
    /* Cleanup grid pool */
    grid_pool_cleanup();

    /* Cleanup move cache structures */
    nfree(move_cache.search_blocks);
    nfree(move_cache.cand_moves);
    move_cache.search_blocks = NULL;
    move_cache.cand_moves = NULL;

    /* Cleanup working grid */
    if (move_cache.initialized) {
        grid_t *working = &move_cache.working_grid;
        nfree(working->stacks);
        nfree(working->relief);
        nfree(working->gaps);
        nfree(working->stack_cnt);
        nfree(working->full_rows);
    }

    move_cache.initialized = false;
    move_cache.size = 0;

    /* Clear caches and statistics */
    clear_eval_cache();
    clear_beam_stats();
    clear_filter_stats();
    clear_depth_stats();

    /* Initialize move ordering system */
    init_move_ordering();
    clear_move_ordering_stats();
}

/* Enhanced Alpha-Beta Search with Advanced Move Ordering
 *
 * This implementation incorporates multiple move ordering heuristics to
 * maximize beta cutoffs and minimize nodes evaluated:
 *
 * 1. Principal Variation: Best move from previous iteration
 * 2. Killer Moves: Moves that caused cutoffs at same depth
 * 3. History Heuristic: Statistically learned move preferences
 * 4. Center-Column Preference: Geometric heuristic fallback
 *
 * The improved ordering typically achieves 70%+ cutoff rates, reducing
 * search tree size by 60-80% compared to naive ordering.
 */
static float ab_search_snapshot(grid_t *working_grid,
                                const shape_stream_t *shapes,
                                const float *weights,
                                int depth,
                                int piece_index,
                                float alpha,
                                float beta,
                                int combo_count)
{
    move_ordering.stats.total_nodes++;

    if (depth <= 0)
        return eval_grid(working_grid, weights, NULL, 0, 0);

    shape_t *shape = shape_stream_peek(shapes, piece_index);
    if (!shape)
        return eval_grid(working_grid, weights, NULL, 0, 0);

    float best = WORST_SCORE;
    block_t blk = {.shape = shape};
    int max_rot = shape->n_rot;
    int elev_y = working_grid->height - shape->max_dim_len;

    /* Generate all legal moves */
    struct move_candidate moves[GRID_WIDTH * 4];
    int move_count = 0;

    for (int rot = 0; rot < max_rot; rot++) {
        int max_cols = working_grid->width - shape->rot_wh[rot].x + 1;

        for (int col = 0; col < max_cols; col++) {
            blk.rot = rot;
            blk.offset.x = col;
            blk.offset.y = elev_y;

            if (!grid_block_collides(working_grid, &blk)) {
                moves[move_count].col = col;
                moves[move_count].rot = rot;
                /* Will be calculated in ordering */
                moves[move_count].score = 0;
                move_count++;
            }
        }
    }

    if (move_count == 0)
        return eval_grid(working_grid, weights, NULL, 0,
                         0); /* No legal moves */

    /* Apply advanced move ordering */
    int current_ply = MAX_SEARCH_DEPTH - depth; /* Convert depth to ply */
    order_moves_advanced(moves, move_count, depth, current_ply);

    /* Search ordered moves */
    for (int i = 0; i < move_count; i++) {
        int col = moves[i].col;
        int rot = moves[i].rot;

        blk.rot = rot;
        blk.offset.x = col;
        blk.offset.y = elev_y;

        /* Store initial Y position for hard drop bonus calculation */
        int initial_y = blk.offset.y;

        /* Apply placement with efficient snapshot system */
        grid_block_drop(working_grid, &blk);

        /* Capture final Y position after drop */
        int final_y = blk.offset.y;

        grid_snapshot_t snap;
        int lines = grid_apply_block(working_grid, &blk, &snap);
        depth_stats.snapshots_used++;

        /* Track snapshot efficiency for performance monitoring */
        if (!snap.needs_full_restore)
            depth_stats.snapshot_efficiency += 1.0f;

        /* Extend search depth for significant line clears */
        int next_depth = depth - 1;
        if (lines >= 2 && depth < MAX_SEARCH_DEPTH)
            next_depth = depth; /* Extend search by not decrementing depth */

        /* Determine combo for the next state */
        int next_combo = (lines > 0) ? combo_count + 1 : 0;

        /* Recurse with alpha-beta bounds and the new combo count */
        float score =
            ab_search_snapshot(working_grid, shapes, weights, next_depth,
                               piece_index + 1, alpha, beta, next_combo);

        /* Optimized bonus calculation */
        float bonus_score = 0.0f;

        /* Enhanced line clear bonus with Tetris emphasis */
        if (lines > 0) {
            float clear_bonus = LINE_CLEAR_BONUS;

            /* Tetris-specific bonuses */
            if (lines == 4) {
                clear_bonus = TETRIS_BONUS; /* Massive Tetris bonus */
            } else if (lines == 3) {
                clear_bonus = TRIPLE_CLEAR_BONUS;
            } else if (lines == 2) {
                clear_bonus = DOUBLE_CLEAR_BONUS;
            }

            /* Quick crisis assessment - only when lines cleared */
            int max_height = working_grid->relief[0] + 1;
            for (int x = 1; x < working_grid->width; x++) {
                int h = working_grid->relief[x] + 1;
                if (h > max_height)
                    max_height = h;
            }

            /* Crisis multiplier for line clear bonus */
            if (max_height >= PANIC_MODE_THRESHOLD) {
                clear_bonus *= 5.0f;
            } else if (max_height >= DESPERATE_MODE_THRESHOLD) {
                clear_bonus *= 3.0f;
            } else if (max_height >= EMERGENCY_MODE_THRESHOLD) {
                clear_bonus *= 2.0f;
            }

            /* Apply line clear bonuses with proper scaling */
            bonus_score += lines * lines * clear_bonus;
        }

        /* Combo bonus for current move */
        if (combo_count > 0)
            bonus_score += COMBO_BONUS * combo_count;

        /* T-spin bonus (only if lines were cleared) */
        if (lines > 0 && is_t_spin(working_grid, &blk))
            bonus_score += T_SPIN_BONUS * lines;

        /* Hard drop bonus (small encouragement for fast placement) */
        float hard_drop_distance = (float) (initial_y - final_y);
        bonus_score += hard_drop_distance * 0.01f;

        score += bonus_score;

        /* Efficient rollback using snapshot system */
        grid_rollback(working_grid, &snap);
        depth_stats.rollbacks_used++;

        /* Update best score */
        if (score > best) {
            best = score;

            /* Update principal variation for next iteration */
            if (current_ply * 2 + 1 < MAX_SEARCH_DEPTH * 2) {
                move_ordering.principal_variation.moves[current_ply * 2] = col;
                move_ordering.principal_variation.moves[current_ply * 2 + 1] =
                    rot;
                if (current_ply == 0) {
                    move_ordering.principal_variation.length = 2;
                    move_ordering.principal_variation.valid = true;
                }
            }
        }

        /* Alpha-beta pruning logic */
        if (score > alpha)
            alpha = score;

        if (alpha >= beta) {
            /* Beta cutoff occurred - update move ordering heuristics */
            move_ordering.stats.total_cutoffs++;

            /* Update history heuristic */
            update_history(depth, col, rot, 1 << (depth + 1));
            move_ordering.stats.history_cutoffs++;

            /* Update killer moves */
            update_killers(depth, col, rot);
            move_ordering.stats.killer_cutoffs++;

            /* Track which heuristic found this move first */
            if (i == 0 && is_pv_move(current_ply, col, rot))
                move_ordering.stats.pv_cutoffs++;

            return best; /* Fail-high: beta cutoff */
        }
    }

    /* Update cutoff rate statistics */
    if (move_ordering.stats.total_nodes > 0) {
        move_ordering.stats.cutoff_rate =
            (float) move_ordering.stats.total_cutoffs /
            move_ordering.stats.total_nodes;
    }

    return best;
}

#if SEARCH_DEPTH >= 2
/* O(1) grid hash using incremental Zobrist hash */
static inline uint64_t grid_hash(const grid_t *g)
{
    return g ? g->hash : 0;
}

/* Tabu list lookup and insertion */
static bool tabu_lookup(uint64_t sig)
{
    size_t idx = sig & (TABU_SIZE - 1);

    if (tabu_seen[idx] == sig && tabu_age[idx] == tabu_current_age)
        return true;

    /* Insert/update entry */
    tabu_seen[idx] = sig;
    tabu_age[idx] = tabu_current_age;
    return false;
}

/* Reset tabu list for new search */
static void tabu_reset(void)
{
    tabu_current_age++;
    /* Age overflow handling */
    if (tabu_current_age == 0) {
        memset(tabu_age, 0, sizeof(tabu_age));
        memset(tabu_seen, 0, sizeof(tabu_seen));
        tabu_current_age = 1;
    }
}
#endif

/* Iterative Deepening Search with Principal Variation Tracking
 *
 * Performs successive searches from depth 1 to target depth.
 * Each iteration refines move ordering using results from previous depths,
 * dramatically improving alpha-beta pruning effectiveness.
 *
 * Benefits:
 * - Better move ordering from shallower searches reduces nodes evaluated
 * - Can return best move at any time if interrupted
 * - Principal variation from each depth improves next iteration
 * - History and killer tables build up progressively
 */
static bool search_best_snapshot(const grid_t *grid,
                                 const shape_stream_t *stream,
                                 const float *weights,
                                 move_t *output,
                                 float *best_score_out)
{
    if (!grid || !stream || !weights || !output || !move_cache.initialized) {
        if (best_score_out)
            *best_score_out = WORST_SCORE;
        return false;
    }

#if SEARCH_DEPTH >= 2
    tabu_reset();
#endif

    /* Detect Tetris-ready well once */
    int well_col = -1;
    bool tetris_ready = grid_is_tetris_ready(grid, &well_col);

    /* Dynamic depth decision - this is our maximum depth */
    int max_search_depth = dynamic_search_depth(grid);

    float current_best_score = WORST_SCORE;
    shape_t *shape = shape_stream_peek(stream, 0);
    if (!shape) {
        if (best_score_out)
            *best_score_out = WORST_SCORE;
        return false;
    }

    block_t *test_block = &move_cache.search_blocks[0];
    grid_t *working_grid = &move_cache.working_grid;

    /* Initialize grid pool if needed */
    if (!grid_pool_init(grid)) {
        if (best_score_out)
            *best_score_out = WORST_SCORE;
        return false;
    }

    /* Use grid pool instead of expensive grid_copy */
    grid_t *pool_grid = grid_pool_acquire();
    if (!pool_grid) {
        /* Fallback to working grid if pool exhausted */
        grid_copy(working_grid, grid);
        pool_grid = working_grid;
    } else {
        grid_fast_copy(pool_grid, grid);
    }
    working_grid = pool_grid;

    test_block->shape = shape;
    output->shape = shape;

    int max_rotations = shape->n_rot;
    int elevated_y = grid->height - shape->max_dim_len;
    test_block->offset.y = elevated_y;

    /* Fast center-out column ordering */
    int order[GRID_WIDTH];
    int ncols = centre_out_order(order, grid->width);

    /* Adaptive beam size */
    int adaptive_beam_size = calc_beam_size(grid);

    /* Beam search - collect candidates */
    beam_candidate_t beam[GRID_WIDTH * 4];
    int beam_count = 0;

    /* Initialize snapshot efficiency tracking */
    float simple_snapshots = 0.0f;
    float total_snapshots = 0.0f;

    /* Phase 1: Collect all candidates using snapshot system with early pruning
     */
    for (int rotation = 0; rotation < max_rotations; rotation++) {
        test_block->rot = rotation;
        int max_columns = grid->width - shape->rot_wh[rotation].x + 1;

        for (int oi = 0; oi < ncols; oi++) {
            int column = order[oi];
            if (column >= max_columns)
                continue;

            test_block->offset.x = column;
            test_block->offset.y = elevated_y;

            if (grid_block_collides(working_grid, test_block))
                continue;

            /* Early pruning: skip obviously poor candidates */
            if (should_skip_evaluation(grid, test_block)) {
                beam_stats.early_pruned++;
                continue;
            }

            /* Store initial Y position for hard drop bonus calculation */
            int initial_y = test_block->offset.y;

            /* Apply placement with efficient snapshot system */
            grid_block_drop(working_grid, test_block);

            /* Capture final Y position after drop */
            int final_y = test_block->offset.y;

            grid_snapshot_t snap;
            int lines_cleared =
                grid_apply_block(working_grid, test_block, &snap);
            depth_stats.snapshots_used++;
            total_snapshots += 1.0f;

            /* Track snapshot efficiency */
            if (!snap.needs_full_restore)
                simple_snapshots += 1.0f;

#if SEARCH_DEPTH >= 2
            /* Check tabu */
            uint64_t grid_sig = grid_hash(working_grid);
            if (tabu_lookup(grid_sig)) {
                grid_rollback(working_grid, &snap);
                depth_stats.rollbacks_used++;
                continue;
            }
#endif

            /* Base evaluation with piece context */
            float position_score = eval_shallow_with_context(
                working_grid, weights, test_block->shape, test_block->rot,
                test_block->offset.x);

            /* Enhanced bonus calculation with Tetris emphasis */
            float placement_bonus = 0.0f;

            /* Tetris-specific line clear bonuses */
            if (lines_cleared == 4) {
                /* Massive Tetris reward */
                placement_bonus += TETRIS_BONUS * 4.0f;
            } else if (lines_cleared == 3) {
                placement_bonus += TRIPLE_CLEAR_BONUS * 3.0f;
            } else if (lines_cleared == 2) {
                placement_bonus += DOUBLE_CLEAR_BONUS * 2.0f;
            } else if (lines_cleared == 1) {
                placement_bonus += LINE_CLEAR_BONUS;
            }

            if (lines_cleared > 0 && is_t_spin(working_grid, test_block))
                placement_bonus += T_SPIN_BONUS * lines_cleared;

            float hard_drop_distance = (float) (initial_y - final_y);
            placement_bonus += hard_drop_distance * 0.01f;

            position_score += placement_bonus;

            /* Enhanced Tetris well management */
            if (tetris_ready) {
                bool is_I_piece = (test_block->shape->rot_wh[0].x == 4);
                int piece_left = test_block->offset.x;
                int piece_right = piece_left +
                                  test_block->shape->rot_wh[test_block->rot].x -
                                  1;

                if (is_I_piece) {
                    /* I-piece placement bonuses */
                    if (test_block->rot == 1 &&
                        test_block->offset.x == well_col) {
                        /* Vertical I-piece in well - perfect for Tetris! */
                        position_score += I_PIECE_WELL_BONUS;
                        if (lines_cleared == 4) {
                            /* Actually achieved a Tetris! */
                            position_score += TETRIS_BONUS * 2.0f;
                        }
                    }
                } else {
                    /* Non-I piece penalties for blocking well */
                    if (piece_right >= well_col && piece_left <= well_col) {
                        int well_depth = working_grid->height -
                                         working_grid->relief[well_col];
                        position_score -= WELL_BLOCK_BASE_PENALTY;
                        position_score -= WELL_BLOCK_DEPTH_FACTOR * well_depth;

                        /* Severe penalty if piece makes well inaccessible */
                        if (!grid_is_well_accessible(working_grid, well_col,
                                                     1)) {
                            position_score -= WELL_ACCESS_BLOCK_PENALTY;
                        }
                    } else if (abs(piece_left - well_col) <= 1 ||
                               abs(piece_right - well_col) <= 1) {
                        /* Bonus for keeping area around well clear */
                        position_score += WELL_MAINTENANCE_BONUS;
                    }
                }
            }

            /* Store candidate */
            if (beam_count < GRID_WIDTH * 4) {
                beam[beam_count++] = (beam_candidate_t) {
                    .rot = rotation,
                    .col = column,
                    .lines = lines_cleared,
                    .shallow = position_score,
                };
            }

            /* Update best */
            if (position_score > current_best_score) {
                current_best_score = position_score;
                output->rot = rotation;
                output->col = column;
            }

            /* Efficient rollback using snapshot system */
            grid_rollback(working_grid, &snap);
            depth_stats.rollbacks_used++;
        }
    }

    /* Update snapshot efficiency statistics */
    if (total_snapshots > 0.0f)
        depth_stats.snapshot_efficiency = simple_snapshots / total_snapshots;

    beam_stats.positions_evaluated += beam_count;

    /* Phase 2: Iterative Deepening with Beam Search
     *
     * Iterative Deepening Depth-First Search (IDDFS) Algorithm:
     * Instead of searching directly to the target depth, we perform
     * successive searches: depth 1, then 2, then 3, up to max_search_depth.
     */
    if (max_search_depth > 1 && beam_count > 0) {
        /* Sort candidates by shallow evaluation for beam selection */
        int effective_beam_size = MIN(beam_count, adaptive_beam_size);
        for (int i = 0; i < effective_beam_size; i++) {
            int best_idx = i;
            for (int j = i + 1; j < beam_count; j++) {
                if (beam[j].shallow > beam[best_idx].shallow)
                    best_idx = j;
            }
            if (best_idx != i) {
                beam_candidate_t temp = beam[i];
                beam[i] = beam[best_idx];
                beam[best_idx] = temp;
            }
        }

        /* Initialize time management for iterative deepening */
        time_manager_init(DEFAULT_TIME_LIMIT_MS);

        /* ITERATIVE DEEPENING: Search progressively deeper */
        for (int current_depth = 1; current_depth <= max_search_depth;
             current_depth++) {
            /* Check time limit before starting new depth */
            if (time_manager_should_stop()) {
                time_manager.early_terminations++;
                break; /* Return best move found so far */
            }

            /* Clear statistics for this iteration */
            int iteration_nodes = move_ordering.stats.total_nodes;

            /* Search each beam candidate at current depth */
            for (int i = 0; i < effective_beam_size; i++) {
                /* Check time during search */
                if (time_manager_should_stop()) {
                    time_manager.early_terminations++;
                    goto search_complete; /* Double break */
                }

                beam_candidate_t *candidate = &beam[i];

                /* Recreate the candidate position */
                test_block->rot = candidate->rot;
                test_block->offset.x = candidate->col;
                test_block->offset.y = elevated_y;

                /* Store initial Y position for hard drop bonus calculation */
                int initial_y = test_block->offset.y;

                grid_block_drop(working_grid, test_block);

                /* Capture final Y position after drop */
                int final_y = test_block->offset.y;

                grid_snapshot_t snap;
                int lines_cleared =
                    grid_apply_block(working_grid, test_block, &snap);
                depth_stats.snapshots_used++;

                /* Alpha-beta search at current iteration depth */
                float alpha = current_best_score * 0.9f;
                float deep_score =
                    ab_search_snapshot(working_grid, stream, weights,
                                       current_depth - 1, 1, alpha, FLT_MAX, 0);

                /* Apply enhanced bonuses with Tetris emphasis */
                float search_bonus = 0.0f;

                /* Tetris-specific line clear bonuses */
                if (lines_cleared == 4) {
                    search_bonus +=
                        TETRIS_BONUS * 4.0f; /* Massive Tetris reward */
                } else if (lines_cleared == 3) {
                    search_bonus += TRIPLE_CLEAR_BONUS * 3.0f;
                } else if (lines_cleared == 2) {
                    search_bonus += DOUBLE_CLEAR_BONUS * 2.0f;
                } else if (lines_cleared == 1) {
                    search_bonus += LINE_CLEAR_BONUS;
                }

                if (lines_cleared > 0 && is_t_spin(working_grid, test_block))
                    search_bonus += T_SPIN_BONUS * lines_cleared;

                float hard_drop_distance = (float) (initial_y - final_y);
                search_bonus += hard_drop_distance * 0.01f;

                deep_score += search_bonus;

                /* Apply enhanced strategic penalties */
                if (tetris_ready) {
                    int pl = candidate->col;
                    int pr = pl + shape->rot_wh[candidate->rot].x - 1;
                    bool is_I_piece = (shape->rot_wh[0].x == 4);
                    if (!is_I_piece && well_col >= pl && well_col <= pr) {
                        /* The piece is blocking the well. Apply a heavy penalty
                         */
                        int well_depth = working_grid->height -
                                         working_grid->relief[well_col];
                        deep_score -= WELL_BLOCK_BASE_PENALTY;
                        deep_score -= WELL_BLOCK_DEPTH_FACTOR * well_depth;

                        /* Check accessibility after this move */
                        if (!grid_is_well_accessible(working_grid, well_col,
                                                     1)) {
                            deep_score -= WELL_ACCESS_BLOCK_PENALTY;
                        }
                    }
                }

                /* Update best move if improved */
                if (deep_score > current_best_score) {
                    current_best_score = deep_score;
                    output->rot = candidate->rot;
                    output->col = candidate->col;

                    /* Update principal variation for next iteration */
                    move_ordering.principal_variation.moves[0] = candidate->col;
                    move_ordering.principal_variation.moves[1] = candidate->rot;
                    move_ordering.principal_variation.length = 2;
                    move_ordering.principal_variation.valid = true;

                    if (current_depth == max_search_depth)
                        beam_stats.beam_hits++;
                }

                /* Restore position */
                grid_rollback(working_grid, &snap);
                depth_stats.rollbacks_used++;
            }

            /* Track nodes evaluated at this depth for analysis */
            int nodes_this_depth =
                move_ordering.stats.total_nodes - iteration_nodes;
            (void) nodes_this_depth; /* Prevent unused variable warning */

            /* Mark depth as completed */
            time_manager.completed_depths = current_depth;
        }
    }

search_complete:
    /* Reset time manager for next search */
    time_manager_reset();

    /* Release grid back to pool if it was acquired */
    if (pool_grid != &move_cache.working_grid)
        grid_pool_release(pool_grid);

    /* Update filter efficiency statistics */
    update_filter_efficiency();

    if (best_score_out)
        *best_score_out = current_best_score;

    return (current_best_score != WORST_SCORE);
}

/* Main interface: find best move for current situation */
move_t *move_find_best(const grid_t *grid,
                       const block_t *current_block,
                       const shape_stream_t *shape_stream,
                       const float *weights)
{
    /* Static cache for move reuse - eliminates malloc/free churn */
    static move_t cached_result;

    if (!grid || !current_block || !shape_stream || !weights)
        return NULL;

    ensure_cleanup();

    /* Initialize move cache */
    if (!cache_init(SEARCH_DEPTH + 1, grid))
        return NULL;

    /* Perform snapshot-based beam search with output buffer */
    float best_score;
    bool success = search_best_snapshot(grid, shape_stream, weights,
                                        &cached_result, &best_score);

    return success ? &cached_result : NULL;
}
