#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tetris.h"
#include "utils.h"

#define WORST_SCORE (-FLT_MAX)

/* Multi-ply search depth with configurable optimizations:
 * - Tabu list: Cache grid hashes to avoid re-evaluating duplicate states
 * - State deduplication: Skip symmetric positions from different move sequences
 * - SEARCH_DEPTH == 1: Greedy evaluation only
 * - SEARCH_DEPTH == 2: Legacy 2-ply search with tabu optimization
 * - SEARCH_DEPTH >= 3: Alpha-beta search with center-out move ordering +
 *                      beam search
 *
 * Complexity: 2-ply ≈400 nodes, 3-ply ≈8,000 nodes (~200 with beam=8)
 */
#define SEARCH_DEPTH 3

/* Beam search configuration */
#define BEAM_SIZE 8        /* Keep top 8 candidates for deep search */
#define BEAM_SIZE_MAX 16   /* Maximum beam size under critical conditions */
#define DANGER_THRESHOLD 4 /* Stack height threshold for danger mode */

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

/* Tall-stack bonus - encourage building up for Tetrises */
#define STACK_HIGH_BONUS 0.40f /* reward per row above threshold */
#define HIGH_STACK_START 10    /* bonus starts when height >= 10 */
#define HIGH_STACK_CAP 17      /* bonus stops growing above height */

/* Well-blocking penalty for non-I pieces on Tetris-ready boards */
#define WELL_BLOCK_PENALTY 2.0f /* score to subtract when plugging well */

/* Terminal position penalty when stack hits ceiling */
#define TOPOUT_PENALTY 10000.0f

/* Alpha-Beta search configuration
 * SEARCH_DEPTH controls total look-ahead plies, including the root move.
 * Depth >= 1: greedily evaluate current placement only.
 * Depth >= 2: alpha-beta over subsequent pieces.
 */

/* Main evaluation cache for complete scores */
#define EVAL_CACHE_SIZE 8192 /* 8K entries ≈64 KiB for better hit rates */

struct eval_cache_entry {
    uint64_t key; /* 64-bit hash: grid + weights */
    float val;    /* cached evaluation */
};

static struct eval_cache_entry eval_cache[EVAL_CACHE_SIZE];

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
};

static struct metrics_entry metrics_cache[METRICS_CACHE_SIZE];

#if SEARCH_DEPTH >= 2
/* Tabu list for avoiding duplicate grid state evaluations */
#define TABU_SIZE 128 /* Power of 2 for fast masking */
static uint64_t tabu_seen[TABU_SIZE];
static uint8_t tabu_age[TABU_SIZE];
static uint8_t tabu_current_age = 0;
#endif

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

/* Evolved weights (Fitness: 1269.00) */
static const float predefined_weights[] = {
    [FEATIDX_RELIEF_MAX] = -1.00f, [FEATIDX_RELIEF_AVG] = -2.78f,
    [FEATIDX_RELIEF_VAR] = -0.65f, [FEATIDX_GAPS] = -2.54f,
    [FEATIDX_OBS] = -1.42f,        [FEATIDX_DISCONT] = -0.03f,
};

/* Move cache during search */
typedef struct {
    grid_t *eval_grids;     /* Grid copies for evaluating different positions */
    block_t *search_blocks; /* Block instances for testing placements */
    move_t *cand_moves;     /* Best moves found at each search depth */
    int size;               /* Number of cached items (equals search depth) */
    bool initialized;       /* Whether cache has been set up */
} move_cache_t;

static move_cache_t move_cache = {0};

/* Cleanup registration flag */
static bool cleanup_registered = false;

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
} beam_stats_t;

static beam_stats_t beam_stats = {0};

/* Cached weights hash */
typedef struct {
    const float *ptr; /* Pointer to weights array (identity check) */
    uint64_t hash;    /* Cached hash of weights */
    bool valid;       /* Whether cache entry is valid */
} weights_cache_t;

static weights_cache_t weights_cache = {0};

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

/* Forward declarations */
static void cache_cleanup(void);

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

    float *weights = malloc(sizeof(predefined_weights));
    if (!weights)
        return NULL;

    memcpy(weights, predefined_weights, sizeof(predefined_weights));
    return weights;
}

/* Calculate grid features, optionally returning bumpiness */
static void calc_features(const grid_t *g, float *features, int *bump_out)
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

    if (bump_out)
        *bump_out = bump;
}

/* Move ordering
 * Fill 'order[]' with column indices starting from the centre column and
 * alternating right/left toward the edges: width=10 -> 5,4,6,3,7,2,8,1,9,0.
 * Returns number of entries written (==width).
 */
static int centre_out_order(int order[], int width)
{
    int centre = width / 2;
    int idx = 0;

    order[idx++] = centre;
    for (int off = 1; off <= centre; ++off) {
        int right = centre + off;
        int left = centre - off;
        if (right < width)
            order[idx++] = right;
        if (left >= 0)
            order[idx++] = left;
    }
    return idx;
}

/* Fast cached hole penalty with early exit on cache hit */
static float get_hole_penalty(const grid_t *g)
{
    if (!g || !g->relief || !g->rows)
        return 0.0f;

    uint32_t idx = g->hash & METRICS_CACHE_MASK;
    struct metrics_entry *entry = &metrics_cache[idx];

    /* Cache hit - return immediately */
    if (entry->grid_key == g->hash)
        return entry->hole_penalty;

    /* Cache miss - compute and store */
    int holes = 0, depth_sum = 0;
    for (int x = 0; x < g->width; x++) {
        int top = g->relief[x];
        if (top < 0 || g->gaps[x] == 0)
            continue;

        for (int y = top - 1; y >= 0; y--) {
            if (!g->rows[y][x]) {
                holes++;
                depth_sum += (top - y);
            }
        }
    }

    float penalty = HOLE_PENALTY * (float) holes +
                    HOLE_PENALTY * HOLE_DEPTH_WEIGHT * (float) depth_sum;

    /* Update cache entry */
    entry->grid_key = g->hash;
    entry->hole_penalty = penalty;

    return penalty;
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
        if (g->n_row_fill[y] == 0) {
            mask = 0;
        } else if (g->n_row_fill[y] == w) {
            mask = (uint16_t) ((1u << w) - 1);
        } else {
            mask = 0;
            for (int x = 0; x < w; x++) {
                if (g->rows[y][x])
                    mask |= (1u << x);
            }
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

/* Evaluation with fast cached expensive metrics */
static float eval_grid(const grid_t *g, const float *weights)
{
    if (!g || !weights)
        return WORST_SCORE;

    /* Fast top-out detection */
    for (int x = 0; x < g->width; x++) {
        if (g->relief[x] >= g->height - 1)
            return -TOPOUT_PENALTY;
    }

    /* Combined hash for complete evaluation cache */
    uint64_t combined_key = g->hash ^ hash_weights(weights);
    combined_key *= 0x2545F4914F6CDD1DULL;

    struct eval_cache_entry *entry =
        &eval_cache[combined_key & (EVAL_CACHE_SIZE - 1)];
    if (entry->key == combined_key)
        return entry->val; /* Complete evaluation cache hit */

    /* Cache miss - compute using fast cached metrics */
    float features[N_FEATIDX];
    calc_features(g, features, NULL);

    /* Calculate weighted score */
    float score = 0.0f;
    for (int i = 0; i < N_FEATIDX; i++)
        score += features[i] * weights[i];

    /* Apply cached expensive heuristics */
    score -= get_hole_penalty(g);
    score -= BUMPINESS_PENALTY * get_bumpiness(g);
    score -= WELL_PENALTY * get_well_depth(g);

    int row_trans, col_trans;
    get_transitions(g, &row_trans, &col_trans);
    score -= ROW_TRANS_PENALTY * row_trans;
    score -= COL_TRANS_PENALTY * col_trans;

    /* Apply remaining lightweight heuristics */
    int total_height = (int) (features[FEATIDX_RELIEF_AVG] * g->width);
    score -= HEIGHT_PENALTY * total_height;

    int max_height = (int) features[FEATIDX_RELIEF_MAX];
    if (max_height >= HIGH_STACK_START) {
        int capped_height =
            (max_height > HIGH_STACK_CAP ? HIGH_STACK_CAP : max_height);
        score += (capped_height - HIGH_STACK_START + 1) * STACK_HIGH_BONUS;
    }

    /* Store in main evaluation cache */
    entry->key = combined_key;
    entry->val = score;

    return score;
}

/* Shallow evaluation with strategic bonuses */
static float eval_shallow(const grid_t *g, const float *weights)
{
    if (!g || !weights)
        return WORST_SCORE;

    /* Base evaluation using cached metrics */
    float base_score = eval_grid(g, weights);

    /* Quick strategic bonuses */
    float bonus = 0.0f;

    /* Simple stability check */
    if (g->relief) {
        int max_height = 0;
        int min_height = g->height;

        for (int x = 0; x < g->width; x++) {
            int h = g->relief[x] + 1;
            if (h > max_height)
                max_height = h;
            if (h < min_height)
                min_height = h;
        }

        /* Reward stable surfaces */
        int height_diff = max_height - min_height;
        if (height_diff <= 3)
            bonus += 0.5f;

        /* Penalty for approaching danger zone */
        if (max_height >= g->height - 4)
            bonus -= (max_height - (g->height - 4)) * 1.0f;
    }

    return base_score + bonus;
}

/* Determine adaptive beam size based on board state */
static int calc_beam_size(const grid_t *g)
{
    if (!g || !g->relief)
        return BEAM_SIZE;

    /* Find maximum stack height */
    int max_height = 0;
    for (int x = 0; x < g->width; x++) {
        int height = g->relief[x] + 1;
        if (height > max_height)
            max_height = height;
    }

    /* Expand beam size when approaching danger zone */
    if (max_height >= g->height - DANGER_THRESHOLD) {
        beam_stats.adaptive_expansions++;
        return BEAM_SIZE_MAX; /* Use larger beam in critical situations */
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

/* Initialize move cache for better performance */
static bool cache_init(int max_depth, const grid_t *template_grid)
{
    if (move_cache.initialized)
        return true;

    move_cache.size = max_depth;
    move_cache.eval_grids = nalloc(max_depth * sizeof(grid_t), NULL);
    move_cache.search_blocks = nalloc(max_depth * sizeof(block_t), NULL);
    move_cache.cand_moves = nalloc(max_depth * sizeof(move_t), NULL);

    if (!move_cache.eval_grids || !move_cache.search_blocks ||
        !move_cache.cand_moves) {
        cache_cleanup();
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
            cache_cleanup();
            return false;
        }

        for (int r = 0; r < template_grid->height; r++) {
            cache_grid->rows[r] =
                ncalloc(template_grid->width, sizeof(*cache_grid->rows[r]),
                        cache_grid->rows);
            if (!cache_grid->rows[r]) {
                cache_cleanup();
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
            cache_cleanup();
            return false;
        }

        for (int c = 0; c < template_grid->width; c++) {
            cache_grid->stacks[c] =
                ncalloc(template_grid->height, sizeof(*cache_grid->stacks[c]),
                        cache_grid->stacks);
            if (!cache_grid->stacks[c]) {
                cache_cleanup();
                return false;
            }
        }
    }

    move_cache.initialized = true;
    return true;
}

/* Clear beam search statistics */
static void clear_beam_stats(void)
{
    memset(&beam_stats, 0, sizeof(beam_stats));
}

/* Cleanup move cache */
static void cache_cleanup(void)
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
    clear_eval_cache();

    /* Clear beam statistics */
    clear_beam_stats();
}

/* Alpha-beta search */
static float ab_search(grid_t *grid,
                       const shape_stream_t *shapes,
                       const float *weights,
                       int depth,
                       int piece_index,
                       float alpha,
                       float beta)
{
    if (depth <= 0)
        return eval_grid(grid, weights);

    shape_t *shape = shape_stream_peek(shapes, piece_index);
    if (!shape)
        return eval_grid(grid, weights);

    /* Fast column ordering - keep the proven center-out approach */
    int order[GRID_WIDTH];
    int ncols = centre_out_order(order, grid->width);

    float best = WORST_SCORE;
    block_t blk = {.shape = shape};
    int max_rot = shape->n_rot;
    int elev_y = grid->height - shape->max_dim_len;

    /* Select eval grid buffer */
    grid_t *child_grid = NULL;
    if (!move_cache.initialized || (piece_index + 1) >= move_cache.size)
        return eval_grid(grid, weights);
    child_grid = &move_cache.eval_grids[piece_index + 1];

    /* Try rotations in reverse order for better pruning */
    for (int rot = max_rot - 1; rot >= 0; --rot) {
        blk.rot = rot;
        int max_cols = grid->width - shape->rot_wh[rot].x + 1;

        for (int oi = 0; oi < ncols; ++oi) {
            int col = order[oi];
            if (col >= max_cols)
                continue;

            blk.offset.x = col;
            blk.offset.y = elev_y;

            if (grid_block_collides(grid, &blk))
                continue;

            /* Prepare child grid */
            grid_copy(child_grid, grid);

            /* Apply placement */
            grid_block_drop(child_grid, &blk);
            grid_block_add(child_grid, &blk);

            int lines = 0;
            if (child_grid->n_full_rows > 0)
                lines = grid_clear_lines(child_grid);

            /* Recurse */
            float score = ab_search(child_grid, shapes, weights, depth - 1,
                                    piece_index + 1, alpha, beta);

            score += lines * LINE_CLEAR_BONUS;

            if (score > best)
                best = score;
            if (score > alpha)
                alpha = score;
            if (alpha >= beta) /* Beta cutoff */
                return best;   /* Early return for better performance */
        }
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

/* Streamlined alpha-beta search for best move */
static move_t *search_best(const grid_t *grid,
                           const shape_stream_t *stream,
                           const float *weights,
                           float *best_score)
{
    if (!grid || !stream || !weights || !move_cache.initialized) {
        *best_score = WORST_SCORE;
        return NULL;
    }

#if SEARCH_DEPTH >= 2
    tabu_reset();
#endif

    /* Detect Tetris-ready well once */
    int well_col = -1;
    bool tetris_ready = grid_is_tetris_ready(grid, &well_col);

    float current_best_score = WORST_SCORE;
    shape_t *shape = shape_stream_peek(stream, 0);
    if (!shape) {
        *best_score = WORST_SCORE;
        return NULL;
    }

    block_t *test_block = &move_cache.search_blocks[0];
    move_t *best_move = &move_cache.cand_moves[0];
    grid_t *eval_grid = &move_cache.eval_grids[0];

    test_block->shape = shape;
    best_move->shape = shape;

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

    /* Phase 1: Collect all candidates */
    for (int rotation = 0; rotation < max_rotations; rotation++) {
        test_block->rot = rotation;
        int max_columns = grid->width - shape->rot_wh[rotation].x + 1;

        for (int oi = 0; oi < ncols; oi++) {
            int column = order[oi];
            if (column >= max_columns)
                continue;

            test_block->offset.x = column;
            test_block->offset.y = elevated_y;

            if (grid_block_collides(grid, test_block))
                continue;

            /* Apply placement */
            grid_block_drop(grid, test_block);
            grid_block_add((grid_t *) grid, test_block);

            /* Determine evaluation grid */
            const grid_t *grid_for_evaluation;
            int lines_cleared = 0;
            if (grid->n_full_rows > 0) {
                grid_copy(eval_grid, grid);
                grid_for_evaluation = eval_grid;
                lines_cleared = grid_clear_lines(eval_grid);
            } else {
                grid_for_evaluation = grid;
            }

#if SEARCH_DEPTH >= 2
            /* Check tabu */
            uint64_t grid_sig = grid_hash(grid_for_evaluation);
            if (tabu_lookup(grid_sig)) {
                grid_block_remove((grid_t *) grid, test_block);
                continue;
            }
#endif

            /* Quick shallow evaluation */
            float position_score = eval_shallow(grid_for_evaluation, weights) +
                                   lines_cleared * LINE_CLEAR_BONUS;

            /* Well-blocking penalty */
            if (tetris_ready) {
                int piece_left = column;
                int piece_right = column + shape->rot_wh[rotation].x - 1;
                bool is_I_piece = (shape->rot_wh[0].x == 4);

                if (!is_I_piece && well_col >= piece_left &&
                    well_col <= piece_right)
                    position_score -= WELL_BLOCK_PENALTY;
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
                best_move->rot = rotation;
                best_move->col = column;
            }

            grid_block_remove((grid_t *) grid, test_block);
        }
    }

    beam_stats.positions_evaluated += beam_count;

    /* Phase 2: Deep search on best candidates */
    if (SEARCH_DEPTH > 1 && beam_count > 0) {
        /* Simple selection sort for top candidates */
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

        for (int i = 0; i < effective_beam_size; i++) {
            beam_candidate_t *candidate = &beam[i];

            /* Recreate placement */
            test_block->rot = candidate->rot;
            test_block->offset.x = candidate->col;
            test_block->offset.y = elevated_y;

            grid_block_drop(grid, test_block);
            grid_block_add((grid_t *) grid, test_block);

            grid_t *grid_for_evaluation;
            if (grid->n_full_rows > 0) {
                grid_copy(eval_grid, grid);
                grid_for_evaluation = eval_grid;
                grid_clear_lines(grid_for_evaluation);
            } else {
                grid_for_evaluation = (grid_t *) grid;
            }

            /* Alpha-beta search with better initial bounds */
            float alpha =
                current_best_score * 0.9f; /* Start closer to current best */
            float deep_score = ab_search(grid_for_evaluation, stream, weights,
                                         SEARCH_DEPTH - 1, 1, alpha, FLT_MAX) +
                               candidate->lines * LINE_CLEAR_BONUS;

            /* Apply well-blocking penalty */
            if (tetris_ready) {
                int pl = candidate->col;
                int pr = pl + shape->rot_wh[candidate->rot].x - 1;
                bool is_I_piece = (shape->rot_wh[0].x == 4);
                if (!is_I_piece && well_col >= pl && well_col <= pr)
                    deep_score -= WELL_BLOCK_PENALTY;
            }

            /* Update best */
            if (deep_score > current_best_score) {
                current_best_score = deep_score;
                best_move->rot = candidate->rot;
                best_move->col = candidate->col;
                beam_stats.beam_hits++;
            }

            grid_block_remove((grid_t *) grid, test_block);
        }
    }

    *best_score = current_best_score;
    return (current_best_score == WORST_SCORE) ? NULL : best_move;
}

/* Main interface: find best move for current situation */
move_t *move_find_best(const grid_t *grid,
                       const block_t *current_block,
                       const shape_stream_t *shape_stream,
                       const float *weights)
{
    if (!grid || !current_block || !shape_stream || !weights)
        return NULL;

    ensure_cleanup();

    /* Initialize move cache with SEARCH_DEPTH+1 grids */
    if (!cache_init(SEARCH_DEPTH + 1, grid))
        return NULL;

    /* Perform beam search */
    float best_score;
    move_t *result = search_best(grid, shape_stream, weights, &best_score);

    return (best_score == WORST_SCORE) ? NULL : result;
}
