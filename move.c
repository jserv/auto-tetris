#include <float.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "nalloc.h"
#include "tetris.h"

#define WORST_SCORE (-FLT_MAX)
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

/* Multi-ply search depth with tabu list optimization:
 * - Tabu list: Cache grid hashes to avoid re-evaluating duplicate states
 * - State deduplication: Skip symmetric positions from different move sequences
 *
 * Complexity: 2-ply ≈400 nodes, 3-ply ≈8,000 nodes (still manageable)
 */
#define SEARCH_DEPTH 2

/* Reward per cleared row */
#define LINE_CLEAR_BONUS 0.75f

/* Penalty per hole (empty cell with filled cell above) */
#define HOLE_PENALTY 0.8f       /* base cost (reduced; depth adds more) */
#define HOLE_DEPTH_WEIGHT 0.05f /* extra cost per covered cell above a hole */

/* Penalty per unit of bumpiness (surface roughness) */
#define BUMPINESS_PENALTY 0.08f

/* Penalty per cell of cumulative well depth */
#define WELL_PENALTY 0.35f

/* Transition penalties - Dellacherie & Böhm heuristic */
#define ROW_TRANS_PENALTY 0.18f /* per horizontal transition */
#define COL_TRANS_PENALTY 0.18f /* per vertical transition */

/* Height penalty - encourage keeping stacks low for reaction time */
#define HEIGHT_PENALTY 0.04f /* per cell of cumulative height */

/* Terminal position penalty when stack hits ceiling */
#define TOPOUT_PENALTY 10000.0f

/* Evaluation cache to avoid re-computing same grid states */
#define HASH_SIZE 4096 /* power of two for cheap masking */

struct cache_entry {
    uint64_t key; /* 64-bit hash of column heights */
    float val;    /* cached evaluation */
};

static struct cache_entry tt[HASH_SIZE]; /* zero-initialized BSS */

/* Tabu list for avoiding duplicate grid state evaluations */
#define TABU_SIZE 128 /* Power of 2 for fast masking */
static uint64_t tabu_seen[TABU_SIZE];
static uint8_t tabu_age[TABU_SIZE];
static uint8_t tabu_current_age = 0;

/* Feature indices for grid evaluation */
enum {
    FEATIDX_RELIEF_MAX = 0, /* Maximum column height */
    FEATIDX_RELIEF_AVG,     /* Average column height */
    FEATIDX_RELIEF_VAR,     /* Variance in column heights */
    FEATIDX_GAPS,           /* Empty cells below blocks */
    FEATIDX_OBS,            /* Total occupied cells */
    FEATIDX_DISCONT,        /* Height discontinuities */
    N_FEATIDX,
};

/* Original weights from predefined system */
static const float predefined_weights[] = {
    [FEATIDX_RELIEF_MAX] = 0.23f,  [FEATIDX_RELIEF_AVG] = -3.62f,
    [FEATIDX_RELIEF_VAR] = -0.21f, [FEATIDX_GAPS] = -0.89f,
    [FEATIDX_OBS] = -0.96f,        [FEATIDX_DISCONT] = -0.27f,
};

/* Return original default weights */
float *default_weights()
{
    float *weights = malloc(sizeof(predefined_weights));
    if (!weights)
        return NULL;

    memcpy(weights, predefined_weights, sizeof(predefined_weights));
    return weights;
}

/* Calculate original grid features */
static void calculate_features(const grid_t *g, float *features)
{
    if (!g || !features) {
        if (features) {
            for (int i = 0; i < N_FEATIDX; i++)
                features[i] = 0.0f;
        }
        return;
    }

    int width = g->width;
    float avg = 0.0f, var = 0.0f, max = 0.0f;
    int discont = -1, last_height = -1;
    int gaps = 0, obs = 0;

    /* Calculate basic metrics following original algorithm */
    for (int i = 0; i < width; i++) {
        int height = g->relief[i];
        if (height > max)
            max = height;
        avg += height + 1;
        discont += (int) (last_height != height);
        last_height = height;

        int cgaps = g->gaps[i];
        gaps += cgaps;
        /* relief[i] is top row index (-1 if empty); +1 gives column height */
        obs += (height + 1) - cgaps;
    }
    avg /= width;

    /* Calculate variance */
    for (int i = 0; i < width; i++) {
        float diff = avg - g->relief[i];
        var += diff * diff;
    }

    /* Store calculated features */
    features[FEATIDX_RELIEF_MAX] = max;
    features[FEATIDX_RELIEF_AVG] = avg;
    features[FEATIDX_RELIEF_VAR] = var;
    features[FEATIDX_DISCONT] = discont;
    features[FEATIDX_GAPS] = gaps;
    features[FEATIDX_OBS] = obs;
}

/* Count "holes": use pre-computed gaps from grid structure */
static inline int count_holes(const grid_t *g)
{
    if (!g || !g->gaps)
        return 0;

    int holes = 0;
    for (int x = 0; x < g->width; x++)
        holes += g->gaps[x];
    return holes;
}

/* Depth-aware hole penalty
 *
 * A "hole" is any empty cell below the topmost filled cell in its column.
 * The deeper the hole (i.e., the more cells covering it), the harder it
 * is to repair. We therefore charge:
 *
 *     penalty = HOLE_PENALTY * holes
 *             + HOLE_PENALTY * HOLE_DEPTH_WEIGHT * depth_sum
 *
 * where depth_sum is the sum, over all holes, of (cover depth).
 */
static inline float advanced_hole_penalty(const grid_t *g)
{
    if (!g || !g->relief || !g->rows)
        return 0.0f;

    int holes = 0;
    int depth_sum = 0;

    for (int x = 0; x < g->width; x++) {
        int top = g->relief[x];         /* -1 if column empty */
        if (top < 0 || g->gaps[x] == 0) /* skip empty columns or no gaps */
            continue;

        /* Scan only cells below the column top */
        for (int y = top - 1; y >= 0; y--) {
            if (!g->rows[y][x]) {
                holes++;
                depth_sum += (top - y); /* cells covering this hole */
            }
        }
    }

    return HOLE_PENALTY * (float) holes +
           HOLE_PENALTY * HOLE_DEPTH_WEIGHT * (float) depth_sum;
}

/* Compute surface "bumpiness": Σ |h[i] - h[i+1]| using grid relief data */
static inline int bumpiness(const grid_t *g)
{
    if (!g || !g->relief)
        return 0;

    int diff_sum = 0;
    for (int x = 0; x < g->width - 1; x++)
        diff_sum += abs((g->relief[x] + 1) - (g->relief[x + 1] + 1));
    return diff_sum;
}

/* One-wide well depth using grid relief data */
static inline int well_depth(const grid_t *g)
{
    if (!g || !g->relief)
        return 0;

    int depth = 0;
    for (int x = 0; x < g->width; x++) {
        int left = (x == 0) ? g->height : (g->relief[x - 1] + 1);
        int right = (x == g->width - 1) ? g->height : (g->relief[x + 1] + 1);
        int height = g->relief[x] + 1;
        if (left > height && right > height)
            depth += MIN(left, right) - height;
    }
    return depth;
}

/* Sum of column heights using grid relief data */
static inline int total_height(const grid_t *g)
{
    if (!g || !g->relief)
        return 0;

    int sum = 0;
    for (int x = 0; x < g->width; x++)
        sum += (g->relief[x] + 1);
    return sum;
}

/* Count transitions from empty→filled or filled→empty along each row.
 * Includes implicit walls on left and right edges.
 */
static int row_transitions(const grid_t *g)
{
    if (!g)
        return 0;

    int transitions = 0;

    for (int y = 0; y < g->height; y++) {
        int prev_filled = 1; /* implicit wall on the left */

        for (int x = 0; x < g->width; x++) {
            int filled = g->rows[y][x] ? 1 : 0;
            if (filled != prev_filled)
                transitions++;
            prev_filled = filled;
        }

        /* Check transition to implicit right wall */
        if (!prev_filled)
            transitions++;
    }

    return transitions;
}

/* Count transitions down each column from bottom to top.
 * Floor is considered filled (implicit bottom wall).
 */
static int col_transitions(const grid_t *g)
{
    if (!g)
        return 0;

    int transitions = 0;

    for (int x = 0; x < g->width; x++) {
        int prev_filled = 1; /* floor is considered filled */

        /* Iterate from bottom to top */
        for (int y = g->height - 1; y >= 0; y--) {
            int filled = g->rows[y][x] ? 1 : 0;
            if (filled != prev_filled)
                transitions++;
            prev_filled = filled;
        }
    }

    return transitions;
}

/* Evaluate grid position using weighted features with fast cache path
 *
 * Uses a three-phase approach for performance:
 * 1. Fast top-out detection: immediate termination for dead positions
 * 2. Compute cheap hash from relief[] + hole count, probe cache
 * 3. On cache miss, compute expensive row/col transitions and full evaluation
 */
static float evaluate_grid(grid_t *g, const float *weights)
{
    if (!g || !weights)
        return WORST_SCORE;

    /* Fast top-out detection: terminate immediately if any column reaches
     * ceiling This prevents wasting computation on terminal positions and
     * avoids misleading scores from positions that cleared lines before topping
     * out
     */
    for (int x = 0; x < g->width; x++) {
        if (g->relief[x] >= g->height - 1)
            return -TOPOUT_PENALTY;
    }

    /* Fast hash computation using precomputed grid data */
    int holes = count_holes(g); /* Sum of gaps across all columns */

    /* FNV-1a hash of column heights and hole count */
    const uint64_t FNV_PRIME = 1099511628211ULL;
    uint64_t h = 14695981039346656037ULL;

    /* Hash column heights: relief[x] ranges from -1 to height-1 */
    for (int x = 0; x < g->width; x++) {
        uint8_t height = (uint8_t) (g->relief[x] + 1);
        h ^= height;
        h *= FNV_PRIME;
    }

    /* Mix in hole count for additional discrimination */
    h ^= holes;
    h *= FNV_PRIME;

    /* Probe evaluation cache */
    struct cache_entry *e = &tt[h & (HASH_SIZE - 1)];
    if (e->key == h)
        return e->val; /* Cache hit: return cached score */

    /* Cache miss: compute full evaluation with expensive operations */
    int row_trans = row_transitions(g);
    int col_trans = col_transitions(g);
    int total_height_val = total_height(g);

    /* Compute features and apply depth-aware penalties */
    float features[N_FEATIDX];
    calculate_features(g, features);

    /* Calculate weighted score */
    float score = 0.0f;
    for (int i = 0; i < N_FEATIDX; i++)
        score += features[i] * weights[i];

    /* Extra heuristic: penalize holes with depth awareness */
    score -= advanced_hole_penalty(g);

    /* Extra heuristic: penalize surface bumpiness */
    score -= BUMPINESS_PENALTY * bumpiness(g);

    /* Extra heuristic: penalize deep wells */
    score -= WELL_PENALTY * well_depth(g);

    /* Transition penalties - Dellacherie & Böhm heuristic */
    score -= ROW_TRANS_PENALTY * row_trans;
    score -= COL_TRANS_PENALTY * col_trans;

    /* Height penalty - encourage keeping stacks low for reaction time */
    score -= HEIGHT_PENALTY * total_height_val;

    /* Store in cache for future look-ups */
    e->key = h;
    e->val = score;

    return score;
}

/* Clear evaluation cache (useful between games or for testing) */
static void clear_evaluation_cache(void)
{
    memset(tt, 0, sizeof(tt));
}

/* Move cache for performance optimization during search */
typedef struct {
    grid_t *eval_grids;     /* Grid copies for evaluating different positions */
    block_t *search_blocks; /* Block instances for testing placements */
    move_t *cand_moves;     /* Best moves found at each search depth */
    int size;               /* Number of cached items (equals search depth) */
    bool initialized;       /* Whether cache has been set up */
} move_cache_t;

static move_cache_t move_cache = {0};

/* Forward declarations */
static void move_cache_cleanup(void);

/* Initialize move cache for better performance */
static bool move_cache_init(int max_depth, const grid_t *template_grid)
{
    if (move_cache.initialized)
        return true;

    move_cache.size = max_depth;
    move_cache.eval_grids = nalloc(max_depth * sizeof(grid_t), NULL);
    move_cache.search_blocks = nalloc(max_depth * sizeof(block_t), NULL);
    move_cache.cand_moves = nalloc(max_depth * sizeof(move_t), NULL);

    if (!move_cache.eval_grids || !move_cache.search_blocks ||
        !move_cache.cand_moves) {
        move_cache_cleanup();
        return false;
    }

    /* Initialize grids */
    for (int i = 0; i < max_depth; i++) {
        grid_t *cache_grid = &move_cache.eval_grids[i];
        *cache_grid = *template_grid; /* Copy structure */

        /* Allocate grid memory */
        cache_grid->rows =
            ncalloc(template_grid->height, sizeof(*cache_grid->rows),
                    move_cache.eval_grids);
        if (!cache_grid->rows) {
            move_cache_cleanup();
            return false;
        }

        for (int r = 0; r < template_grid->height; r++) {
            cache_grid->rows[r] =
                ncalloc(template_grid->width, sizeof(*cache_grid->rows[r]),
                        cache_grid->rows);
            if (!cache_grid->rows[r]) {
                move_cache_cleanup();
                return false;
            }
        }

        /* Allocate other grid arrays */
        cache_grid->stacks =
            ncalloc(template_grid->width, sizeof(*cache_grid->stacks),
                    move_cache.eval_grids);
        cache_grid->relief =
            ncalloc(template_grid->width, sizeof(*cache_grid->relief),
                    move_cache.eval_grids);
        cache_grid->gaps =
            ncalloc(template_grid->width, sizeof(*cache_grid->gaps),
                    move_cache.eval_grids);
        cache_grid->stack_cnt =
            ncalloc(template_grid->width, sizeof(*cache_grid->stack_cnt),
                    move_cache.eval_grids);
        cache_grid->n_row_fill =
            ncalloc(template_grid->height, sizeof(*cache_grid->n_row_fill),
                    move_cache.eval_grids);
        cache_grid->full_rows =
            ncalloc(template_grid->height, sizeof(*cache_grid->full_rows),
                    move_cache.eval_grids);

        if (!cache_grid->stacks || !cache_grid->relief || !cache_grid->gaps ||
            !cache_grid->stack_cnt || !cache_grid->n_row_fill ||
            !cache_grid->full_rows) {
            move_cache_cleanup();
            return false;
        }

        for (int c = 0; c < template_grid->width; c++) {
            cache_grid->stacks[c] =
                ncalloc(template_grid->height, sizeof(*cache_grid->stacks[c]),
                        cache_grid->stacks);
            if (!cache_grid->stacks[c]) {
                move_cache_cleanup();
                return false;
            }
        }
    }

    move_cache.initialized = true;
    return true;
}

/* Cleanup move cache */
static void move_cache_cleanup(void)
{
    if (move_cache.eval_grids) {
        nfree(move_cache.eval_grids);
        move_cache.eval_grids = NULL;
    }
    if (move_cache.search_blocks) {
        nfree(move_cache.search_blocks);
        move_cache.search_blocks = NULL;
    }
    if (move_cache.cand_moves) {
        nfree(move_cache.cand_moves);
        move_cache.cand_moves = NULL;
    }
    move_cache.initialized = false;
    move_cache.size = 0;

    /* Clear evaluation cache as well */
    clear_evaluation_cache();
}

/* Register cleanup function */
void move_cleanup_atexit(void)
{
    atexit(move_cache_cleanup);
}

/* Rotate 64-bit word left by k positions, handling k=0 safely */
static inline uint64_t rotl64(uint64_t x, unsigned k)
{
    return (x << k) | (x >> (64 - k));
}

/* Generate hash signature from grid cell occupancy for tabu list */
static uint64_t grid_hash(const grid_t *g)
{
    if (!g)
        return 0;

    uint64_t hash = 0;
    uint64_t bit_pos = 0;

    /* Create 64-bit signature from top 20 rows using row occupancy patterns
     * Each row contributes a bit pattern, rotated by varying amounts to
     * distribute hash values and minimize collisions between similar grids
     */
    for (int row = 0; row < g->height && row < 20; row++) {
        uint64_t row_bits = 0;
        for (int col = 0; col < g->width && col < GRID_WIDTH; col++) {
            if (g->rows[row][col])
                row_bits |= (1ULL << col);
        }

        /* XOR-fold with rotation to spread bit patterns */
        unsigned shift = bit_pos & 63; /* Mask to avoid undefined behavior */
        hash ^= (shift ? rotl64(row_bits, shift) : row_bits);
        bit_pos += 7; /* Prime offset for good distribution */
    }

    return hash;
}

/* Tabu list lookup and insertion */
static bool tabu_lookup(uint64_t sig)
{
    size_t idx = sig & (TABU_SIZE - 1); /* Power-of-two mask */

    if (tabu_seen[idx] == sig && tabu_age[idx] == tabu_current_age)
        return true; /* Already explored in this search */

    /* Insert/update entry */
    tabu_seen[idx] = sig;
    tabu_age[idx] = tabu_current_age;
    return false;
}

/* Reset tabu list for new search */
static void tabu_reset(void)
{
    tabu_current_age++;
    /* Age overflow handling - clear table if needed */
    if (tabu_current_age == 0) {
        memset(tabu_age, 0, sizeof(tabu_age));
        memset(tabu_seen, 0, sizeof(tabu_seen));
        tabu_current_age = 1;
    }
}

/* 2-ply search with tabu list (hash cache) */
static float search_next_piece(grid_t *current_grid,
                               shape_stream_t *shape_stream,
                               const float *weights,
                               int piece_index,
                               int max_relief_height)
{
    /* Check tabu list first */
    uint64_t grid_sig = grid_hash(current_grid);
    if (tabu_lookup(grid_sig)) {
        /* Already evaluated this grid state, return cached estimate */
        return evaluate_grid(current_grid, weights);
    }

    /* Get the next piece to evaluate */
    shape_t *next_shape = shape_stream_peek(shape_stream, piece_index);
    if (!next_shape)
        return evaluate_grid(current_grid, weights);

    float best_next_score = WORST_SCORE;
    block_t search_block;
    /* Use second grid for next piece */
    grid_t *temp_grid = &move_cache.eval_grids[1];

    /* Initialize search block for next piece */
    search_block.shape = next_shape;
    int max_rotations = next_shape->n_rot;
    bool fast_placement = (current_grid->height - 1 - max_relief_height) >=
                          next_shape->max_dim_len;
    int elevated_y = current_grid->height - next_shape->max_dim_len;

    /* Try all possible placements for next piece */
    for (int rotation = 0; rotation < max_rotations; rotation++) {
        search_block.rot = rotation;
        int max_columns =
            current_grid->width - next_shape->rot_wh[rotation].x + 1;
        search_block.offset.y = elevated_y;

        for (int column = 0; column < max_columns; column++) {
            search_block.offset.x = column;
            search_block.offset.y = elevated_y;

            /* Check if placement is valid */
            if (!fast_placement &&
                grid_block_intersects(current_grid, &search_block))
                continue;

            /* Copy current grid to temp grid */
            grid_cpy(temp_grid, current_grid);

            /* Apply next piece move to temp grid */
            grid_block_drop(temp_grid, &search_block);
            grid_block_add(temp_grid, &search_block);

            /* Clear lines and record how many were removed */
            int lines_cleared = 0;
            if (temp_grid->n_full_rows > 0)
                lines_cleared = grid_clear_lines(temp_grid);

            /* Evaluate grid + bonus */
            float next_score = evaluate_grid(temp_grid, weights) +
                               lines_cleared * LINE_CLEAR_BONUS;

            /* Update best score for next piece */
            if (next_score > best_next_score)
                best_next_score = next_score;
        }
    }

    return best_next_score;
}

/* Tabu-optimized search for best move */
static move_t *search_best_move(grid_t *current_grid,
                                shape_stream_t *shape_stream,
                                const float *weights,
                                float *best_score,
                                int max_relief_height)
{
    if (!current_grid || !shape_stream || !weights || !move_cache.initialized) {
        *best_score = WORST_SCORE;
        return NULL;
    }

    /* Reset tabu list for new search */
    tabu_reset();

    float current_best_score = WORST_SCORE;

    shape_t *current_shape = shape_stream_peek(shape_stream, 0);
    if (!current_shape) {
        *best_score = WORST_SCORE;
        return NULL;
    }

    block_t *search_block = &move_cache.search_blocks[0];
    move_t *best_move = &move_cache.cand_moves[0];
    grid_t *evaluation_grid = &move_cache.eval_grids[0];

    /* Initialize search block */
    search_block->shape = current_shape;
    best_move->shape = current_shape;

    int max_rotations = current_shape->n_rot;
    bool fast_placement = (current_grid->height - 1 - max_relief_height) >=
                          current_shape->max_dim_len;
    int elevated_y = current_grid->height - current_shape->max_dim_len;

    search_block->offset.y = elevated_y;

    /* Try all possible placements for current piece */
    for (int rotation = 0; rotation < max_rotations; rotation++) {
        search_block->rot = rotation;
        int max_columns =
            current_grid->width - current_shape->rot_wh[rotation].x + 1;
        search_block->offset.y = elevated_y;

        for (int column = 0; column < max_columns; column++) {
            search_block->offset.x = column;
            search_block->offset.y = elevated_y;

            /* Check if placement is valid */
            if (!fast_placement &&
                grid_block_intersects(current_grid, search_block))
                continue;

            /* Drop block to final position */
            grid_block_drop(current_grid, search_block);
            grid_block_add(current_grid, search_block);

            /* Determine which grid to use for evaluation */
            grid_t *grid_for_evaluation;

            int lines_cleared = 0;
            if (current_grid->n_full_rows > 0) {
                /* Copy grid and clear lines */
                grid_for_evaluation = evaluation_grid;
                grid_cpy(grid_for_evaluation, current_grid);
                lines_cleared = grid_clear_lines(grid_for_evaluation);
            } else {
                grid_for_evaluation = current_grid;
            }

            /* Multi-ply evaluation with tabu optimization */
            float position_score;
            if (SEARCH_DEPTH > 1) {
                /* 2-ply with tabu list optimization */
                position_score =
                    search_next_piece(grid_for_evaluation, shape_stream,
                                      weights, 1, max_relief_height) +
                    lines_cleared * LINE_CLEAR_BONUS;
            } else {
                /* Classic greedy: evaluate current position only */
                position_score = evaluate_grid(grid_for_evaluation, weights) +
                                 lines_cleared * LINE_CLEAR_BONUS;
            }

            /* Update best move if this is better */
            if (position_score > current_best_score) {
                current_best_score = position_score;
                best_move->rot = rotation;
                best_move->col = column;
            }

            /* Undo block placement */
            grid_block_remove(current_grid, search_block);
        }
    }

    *best_score = current_best_score;
    return (current_best_score == WORST_SCORE) ? NULL : best_move;
}

/* Main interface: find best move for current situation */
move_t *best_move(grid_t *grid,
                  block_t *current_block,
                  shape_stream_t *shape_stream,
                  float *weights)
{
    if (!grid || !current_block || !shape_stream || !weights)
        return NULL;

    /* Initialize move cache with 2 grids (current + next piece evaluation) */
    if (!move_cache_init(2, grid))
        return NULL;

    /* Find maximum relief height for optimization */
    int max_relief = -1;
    for (int i = 0; i < grid->width; i++)
        max_relief = MAX(max_relief, grid->relief[i]);

    /* Perform search (1-ply or multi-ply based on SEARCH_DEPTH) */
    float best_score;
    move_t *result =
        search_best_move(grid, shape_stream, weights, &best_score, max_relief);

    return (best_score == WORST_SCORE) ? NULL : result;
}
