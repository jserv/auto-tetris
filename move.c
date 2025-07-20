#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nalloc.h"
#include "tetris.h"

/* Compiler-specific popcount implementation */
#if defined(__GNUC__) || defined(__clang__)
/* GCC and Clang have __builtin_popcount */
#define POPCOUNT(x) __builtin_popcount(x)
#elif defined(_MSC_VER)
/* Microsoft Visual C++ */
#include <intrin.h>
#define POPCOUNT(x) __popcnt(x)
#else
/* Fallback implementation using SWAR (SIMD Within A Register) technique */
static inline int popcount_fallback(uint16_t x)
{
    int count = 0;
    while (x) {
        x &= x - 1; /* Clear the lowest set bit */
        count++;
    }
    return count;
}
#define POPCOUNT(x) popcount_fallback(x)
#endif

#define WORST_SCORE (-FLT_MAX)
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

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

/* Terminal position penalty when stack hits ceiling */
#define TOPOUT_PENALTY 10000.0f

/* Alpha-Beta search configuration
 * SEARCH_DEPTH controls total look-ahead plies, including the root move.
 * Depth >= 1: greedily evaluate current placement only.
 * Depth >= 2: alpha-beta over subsequent pieces.
 */

/* Evaluation cache to avoid re-computing same grid states */
#define HASH_SIZE 8192 /* 8K entries ≈64 KiB for better hit rates */

struct cache_entry {
    uint64_t key; /* 64-bit hash of column heights */
    float val;    /* cached evaluation */
};

static struct cache_entry tt[HASH_SIZE]; /* zero-initialized BSS */

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

/* Move cache for performance optimization during search */
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
    bool tabu;     /* Whether this position is in tabu list */
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

/* Forward declarations */
static void move_cache_cleanup(void);

/* Ensure cleanup is registered when move module is first used */
static void ensure_cleanup_registered(void)
{
    if (!cleanup_registered) {
        atexit(move_cache_cleanup);
        cleanup_registered = true;
    }
}

/* Return default weights */
float *move_default_weights()
{
    ensure_cleanup_registered();

    float *weights = malloc(sizeof(predefined_weights));
    if (!weights)
        return NULL;

    memcpy(weights, predefined_weights, sizeof(predefined_weights));
    return weights;
}

/* Calculate grid features */
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

    /* Calculate basic metrics */
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

    /* Calculate variance (avg is in cells; relief[] is 0-based index) */
    for (int i = 0; i < width; i++) {
        float diff = avg - (g->relief[i] + 1);
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

/* Move ordering
 * Fill 'order[]' with column indices starting from the centre column and
 * alternating right/left toward the edges: width=10 -> 5,4,6,3,7,2,8,1,9,0.
 * Returns number of entries written (==width).
 */
static inline int centre_out_order(int order[], int width)
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

/* Bit-packed row and column transition counting using uint16_t masks.
 *
 * Board width ≤16 → fits in uint16_t. We build a mask for each row:
 *   bit i == 1 ⇔ cell(i) filled.
 *
 * Row transitions:
 *   - left wall vs first cell: (1 ^ first)
 *   - popcount(mask ^ (mask >> 1)) between adjacent cells
 *   - last cell vs right wall: (1 ^ last)
 *
 * Column transitions:
 *   - popcount(mask ^ prev_mask) (prev_mask initialized to all 1s = floor)
 *
 * One pass = two popcounts per row; branch-free inner loop.
 * Scans bottom-to-top to maintain correct column transition semantics.
 */
static void rowcol_transitions(const grid_t *g, int *row_out, int *col_out)
{
    if (!g) {
        if (row_out)
            *row_out = 0;
        if (col_out)
            *col_out = 0;
        return;
    }

    int row_t = 0, col_t = 0;
    uint16_t prev_mask = 0xFFFF; /* floor: all filled */
    const int w = g->width;      /* ≤14, fits in 16 bits */

    /* Scan grid bottom-to-top (required for correct column transitions) */
    for (int y = g->height - 1; y >= 0; y--) {
        uint16_t mask = 0;

        /* Build row mask: bit i set ⇔ cell(y,i) filled */
        for (int x = 0; x < w; x++)
            if (g->rows[y][x])
                mask |= (1u << x);

        /* Row transitions: count horizontal changes */
        int first = mask & 1u;
        int last = (mask >> (w - 1)) & 1u;

        row_t += (1 ^ first); /* left wall vs first cell */

        uint16_t diff = mask ^ (mask >> 1);
        /* mask out spurious high bit */
        diff &= (uint16_t) ((1u << (w - 1)) - 1);
        row_t += POPCOUNT(diff); /* adjacent cell transitions */

        row_t += (1 ^ last); /* last cell vs right wall */

        /* Column transitions: count vertical changes */
        col_t += POPCOUNT(prev_mask ^ mask);
        prev_mask = mask;
    }

    if (row_out)
        *row_out = row_t;
    if (col_out)
        *col_out = col_t;
}

/* Evaluate grid position using weighted features with fast cache path
 *
 * Uses a three-phase approach for performance:
 * 1. Fast top-out detection: immediate termination for dead positions
 * 2. Compute cheap hash from relief[] + hole count, probe cache
 * 3. On cache miss, compute expensive row/col transitions and full evaluation
 */
static float evaluate_grid(const grid_t *g, const float *weights)
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

    /* Cache miss: compute transitions in single pass for better performance */
    int row_trans, col_trans;
    rowcol_transitions(g, &row_trans, &col_trans);

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

/* Detect potential T-spin setup opportunities */
static float detect_tspin_setups(const grid_t *g)
{
    if (!g || !g->relief || !g->rows)
        return 0.0f;

    float setup_bonus = 0.0f;

    /* Look for T-spin shapes: 3-wide gaps with proper height differential */
    for (int x = 1; x < g->width - 1; x++) {
        int left_height = g->relief[x - 1] + 1;
        int mid_height = g->relief[x] + 1;
        int right_height = g->relief[x + 1] + 1;

        /* Classic T-spin double setup: sides higher than middle by 2+ */
        if (left_height >= mid_height + 2 && right_height >= mid_height + 2) {
            /* Check for proper T-slot shape */
            int target_row = mid_height;
            if (target_row < g->height - 1 && !g->rows[target_row][x] &&
                !g->rows[target_row + 1][x]) {
                setup_bonus += 1.5f; /* Reward T-spin opportunities */
            }
        }

        /* T-spin single setups: one side higher */
        if ((left_height == mid_height + 2 && right_height == mid_height + 1) ||
            (left_height == mid_height + 1 && right_height == mid_height + 2)) {
            setup_bonus += 0.8f;
        }
    }

    return setup_bonus;
}

/* Assess immediate stack danger */
static float stack_danger_assessment(const grid_t *g)
{
    if (!g || !g->relief)
        return 0.0f;

    float danger_score = 0.0f;

    /* Calculate maximum stack height */
    int max_height = 0;
    for (int x = 0; x < g->width; x++) {
        int height = g->relief[x] + 1;
        if (height > max_height)
            max_height = height;
    }

    /* Exponential penalty as we approach ceiling */
    if (max_height > g->height - 6) {
        float height_ratio = (float) (max_height - (g->height - 6)) / 6.0f;
        danger_score =
            height_ratio * height_ratio * 10.0f; /* Quadratic penalty */
    }

    /* Penalty for narrow spires that limit piece placement */
    for (int x = 0; x < g->width; x++) {
        int height = g->relief[x] + 1;
        if (height > g->height - 4) {
            /* Check if this is an isolated spire */
            int left_height = (x > 0) ? g->relief[x - 1] + 1 : 0;
            int right_height = (x < g->width - 1) ? g->relief[x + 1] + 1 : 0;

            if (height - left_height >= 3 && height - right_height >= 3)
                danger_score += 3.0f; /* Penalty for dangerous spires */
        }
    }

    return danger_score;
}

/* Check for board clearing potential (Tetris opportunities) */
static float tetris_opportunity_bonus(const grid_t *g)
{
    if (!g || !g->relief || !g->rows)
        return 0.0f;

    /* Look for 4-line Tetris setups */
    for (int x = 0; x < g->width; x++) {
        int height = g->relief[x] + 1;

        /* Check if this column is significantly lower and forms a well */
        bool is_well = true;
        int well_depth = 0;

        /* Check neighbors to see if this forms a deep well */
        for (int neighbor = 0; neighbor < g->width; neighbor++) {
            if (neighbor == x)
                continue;

            int neighbor_height = g->relief[neighbor] + 1;
            if (neighbor_height < height + 3) {
                is_well = false;
                break;
            }
            well_depth = neighbor_height - height;
        }

        /* Reward deep wells that can accommodate I-pieces */
        if (is_well && well_depth >= 4) {
            /* Check if the well is actually clear */
            bool well_clear = true;
            for (int y = height; y < height + 4 && y < g->height; y++) {
                if (g->rows[y][x]) {
                    well_clear = false;
                    break;
                }
            }

            if (well_clear)
                return 2.0f; /* Strong bonus for Tetris opportunities */
        }
    }

    return 0.0f;
}

/* Enhanced shallow evaluation with position-aware features */
static float shallow_evaluate_grid(const grid_t *g, const float *weights)
{
    if (!g || !weights)
        return WORST_SCORE;

    /* Base evaluation using standard features */
    float base_score = evaluate_grid(g, weights);

    /* Add shallow-specific strategic bonuses */
    float setup_bonus = detect_tspin_setups(g) * 0.5f;
    float tetris_bonus = tetris_opportunity_bonus(g) * 0.8f;
    float danger_penalty = stack_danger_assessment(g) * -1.5f;

    /* Reward board stability */
    float stability_bonus = 0.0f;
    if (g->relief) {
        int height_variance = 0;
        int avg_height = 0;

        for (int x = 0; x < g->width; x++)
            avg_height += g->relief[x] + 1;
        avg_height /= g->width;

        for (int x = 0; x < g->width; x++) {
            int diff = (g->relief[x] + 1) - avg_height;
            height_variance += diff * diff;
        }

        /* Bonus for stable, level surfaces */
        if (height_variance < g->width * 2)
            stability_bonus = 1.0f;
    }

    return base_score + setup_bonus + tetris_bonus + danger_penalty +
           stability_bonus;
}

/* Determine adaptive beam size based on board state */
static int calculate_adaptive_beam_size(const grid_t *g)
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

    /* Check for complex board states that might benefit from larger beam */
    int height_variance = 0;
    int avg_height = 0;

    for (int x = 0; x < g->width; x++)
        avg_height += g->relief[x] + 1;
    avg_height /= g->width;

    for (int x = 0; x < g->width; x++) {
        int diff = (g->relief[x] + 1) - avg_height;
        height_variance += diff * diff;
    }

    /* Expand beam for highly irregular surfaces */
    if (height_variance > g->width * 8)
        return MIN(BEAM_SIZE + 4, BEAM_SIZE_MAX);

    return BEAM_SIZE;
}

/* Clear evaluation cache (useful between games or for testing) */
static void clear_evaluation_cache(void)
{
    memset(tt, 0, sizeof(tt));
}

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

/* Clear beam search statistics */
static void clear_beam_stats(void)
{
    memset(&beam_stats, 0, sizeof(beam_stats));
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

    /* Clear beam statistics */
    clear_beam_stats();
}

/* Alpha-beta search (single-player maximization)
 * - depth counts remaining plies including this call's piece.
 * - alpha/beta are running bounds in score space (maximization only).
 * - piece_index selects which upcoming piece to place (0 == next piece after
 *   the root move).
 * - The function re-uses move_cache.eval_grids[piece_index+1] for child grids
 *   when available; otherwise it falls back to leaf evaluation.
 *
 * NOTE: Tabu is not applied below the root; alpha-beta provides pruning and
 * tabu bookkeeping per-node becomes a measurable cost. Root search still uses
 * tabu_reset()/tabu_lookup() in search_move_best().
 */
static float ab_search(grid_t *grid,
                       shape_stream_t *shapes,
                       const float *weights,
                       int depth,
                       int piece_index,
                       float alpha,
                       float beta)
{
    if (depth <= 0)
        return evaluate_grid(grid, weights);

    shape_t *shape = shape_stream_peek(shapes, piece_index);
    if (!shape)
        return evaluate_grid(grid, weights);

    /* Column ordering */
    int order[GRID_WIDTH]; /* GRID_WIDTH from tetris.h */
    int ncols = centre_out_order(order, grid->width);

    float best = WORST_SCORE;
    block_t blk;
    blk.shape = shape;

    int max_rot = shape->n_rot;
    int elev_y = grid->height - shape->max_dim_len;

    /* Select eval grid buffer: use cached grid if available */
    grid_t *child_grid = NULL;
    if (!move_cache.initialized || (piece_index + 1) >= move_cache.size)
        return evaluate_grid(grid, weights); /* Safe fallback */
    child_grid = &move_cache.eval_grids[piece_index + 1];

    for (int rot = 0; rot < max_rot; ++rot) {
        blk.rot = rot;
        int max_cols = grid->width - shape->rot_wh[rot].x + 1;

        for (int oi = 0; oi < ncols; ++oi) {
            int col = order[oi];
            if (col >= max_cols)
                continue; /* piece extends past right edge in this rot */

            blk.offset.x = col;
            blk.offset.y = elev_y;

            /* Always verify legality */
            if (grid_block_intersects(grid, &blk))
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

            /* Add per-move bonus for cleared rows */
            score += lines * LINE_CLEAR_BONUS;

            if (score > best)
                best = score;
            if (score > alpha)
                alpha = score;
            if (alpha >= beta) /* Beta cutoff */
                goto done;
        }
    }

done:
    return best;
}

#if SEARCH_DEPTH >= 2
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
#endif /* SEARCH_DEPTH >= 2 */

/* Alpha-beta search for best move with beam search optimization */
static move_t *search_move_best(grid_t *current_grid,
                                shape_stream_t *shape_stream,
                                const float *weights,
                                float *best_score)
{
    if (!current_grid || !shape_stream || !weights || !move_cache.initialized) {
        *best_score = WORST_SCORE;
        return NULL;
    }

#if SEARCH_DEPTH >= 2
    /* Reset tabu list for new search */
    tabu_reset();
#endif

    float current_best_score = WORST_SCORE;

    shape_t *current_shape = shape_stream_peek(shape_stream, 0);
    if (!current_shape) {
        *best_score = WORST_SCORE;
        return NULL;
    }

    block_t *search_block = &move_cache.search_blocks[0];
    move_t *move_best = &move_cache.cand_moves[0];
    grid_t *evaluation_grid = &move_cache.eval_grids[0];

    /* Initialize search block */
    search_block->shape = current_shape;
    move_best->shape = current_shape;

    int max_rotations = current_shape->n_rot;
    int elevated_y = current_grid->height - current_shape->max_dim_len;

    search_block->offset.y = elevated_y;

    /* Use center-out column ordering for consistent move ordering */
    int order[GRID_WIDTH];
    int ncols = centre_out_order(order, current_grid->width);

    /* Determine adaptive beam size based on board state */
    int adaptive_beam_size = calculate_adaptive_beam_size(current_grid);

    /* Beam search: collect all candidates with shallow scores first */
    beam_candidate_t beam[GRID_WIDTH * 4];
    int beam_count = 0;

    /* Phase 1: Enumerate all legal placements */
    for (int rotation = 0; rotation < max_rotations; rotation++) {
        search_block->rot = rotation;
        int max_columns =
            current_grid->width - current_shape->rot_wh[rotation].x + 1;
        search_block->offset.y = elevated_y;

        for (int oi = 0; oi < ncols; oi++) {
            int column = order[oi];
            if (column >= max_columns)
                continue; /* piece extends past right edge in this rotation */

            search_block->offset.x = column;
            search_block->offset.y = elevated_y;

            /* Always check placement legality */
            if (grid_block_intersects(current_grid, search_block))
                continue;

            /* Drop block to final position */
            grid_block_drop(current_grid, search_block);
            grid_block_add(current_grid, search_block);

            /* Determine which grid to use for evaluation */
            grid_t *grid_for_evaluation;
            int lines_cleared = 0;
            if (current_grid->n_full_rows > 0) {
                /* Copy grid and clear lines */
                grid_copy(evaluation_grid, current_grid);
                grid_for_evaluation = evaluation_grid;
                lines_cleared = grid_clear_lines(grid_for_evaluation);
            } else {
                grid_for_evaluation = current_grid;
            }

#if SEARCH_DEPTH >= 2
            /* Check tabu list to avoid re-evaluating duplicate states */
            uint64_t grid_sig = grid_hash(grid_for_evaluation);
            if (tabu_lookup(grid_sig)) {
                /* Already evaluated this grid state, skip to next move */
                grid_block_remove(current_grid, search_block);
                continue;
            }
#endif

            /* Compute shallow score with strategic awareness */
            float position_score =
                shallow_evaluate_grid(grid_for_evaluation, weights) +
                lines_cleared * LINE_CLEAR_BONUS;

            /* Store candidate for beam search */
            if (beam_count < GRID_WIDTH * 4) {
                beam[beam_count++] = (beam_candidate_t) {
                    .rot = rotation,
                    .col = column,
                    .lines = lines_cleared,
                    .shallow = position_score,
                    .tabu = false,
                };
            }

            /* Update best with shallow score */
            if (position_score > current_best_score) {
                current_best_score = position_score;
                move_best->rot = rotation;
                move_best->col = column;
            }

            /* Undo block placement */
            grid_block_remove(current_grid, search_block);
        }
    }

    /* Update performance statistics */
    beam_stats.positions_evaluated += beam_count;

    /* Phase 2: For multi-ply search, select top candidates for deep search */
    if (SEARCH_DEPTH > 1 && beam_count > 0) {
        /* Sort beam by shallow score (partial selection sort for top
         * candidates) */
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

        /* Track the shallow score of the best candidate for performance
         * analysis
         */
        float shallow_best_score = current_best_score;

        /* Deep search only top beam candidates */
        for (int i = 0; i < effective_beam_size; i++) {
            beam_candidate_t *candidate = &beam[i];

            /* Recreate the placement */
            search_block->rot = candidate->rot;
            search_block->offset.x = candidate->col;
            search_block->offset.y = elevated_y;

            /* Apply placement exactly as in phase 1 */
            grid_block_drop(current_grid, search_block);
            grid_block_add(current_grid, search_block);

            /* Determine evaluation grid (same logic as phase 1) */
            grid_t *grid_for_evaluation;
            if (current_grid->n_full_rows > 0) {
                grid_copy(evaluation_grid, current_grid);
                grid_for_evaluation = evaluation_grid;
                grid_clear_lines(grid_for_evaluation);
            } else {
                grid_for_evaluation = current_grid;
            }

            /* Run deep alpha-beta search */
            float deep_score =
                ab_search(grid_for_evaluation, shape_stream, weights,
                          SEARCH_DEPTH - 1, 1, WORST_SCORE, FLT_MAX) +
                candidate->lines * LINE_CLEAR_BONUS;

            /* Update best move if deep search found better result */
            if (deep_score > current_best_score) {
                current_best_score = deep_score;
                move_best->rot = candidate->rot;
                move_best->col = candidate->col;

                /* Track if the best move was in our beam */
                beam_stats.beam_hits++;
            }

            /* Undo placement */
            grid_block_remove(current_grid, search_block);
        }

        /* Check if we improved over shallow evaluation */
        if (current_best_score > shallow_best_score) {
            float improvement = current_best_score - shallow_best_score;
            beam_stats.avg_beam_score_diff =
                (beam_stats.avg_beam_score_diff * beam_stats.beam_hits +
                 improvement) /
                (beam_stats.beam_hits + 1);
        }

        /* Check if best move was outside our beam (missed optimization) */
        bool best_in_beam = false;
        for (int i = 0; i < effective_beam_size; i++) {
            if (beam[i].rot == move_best->rot &&
                beam[i].col == move_best->col) {
                best_in_beam = true;
                break;
            }
        }
        if (!best_in_beam)
            beam_stats.beam_misses++;
    }

    *best_score = current_best_score;
    return (current_best_score == WORST_SCORE) ? NULL : move_best;
}

/* Main interface: find best move for current situation */
move_t *move_best(grid_t *grid,
                  block_t *current_block,
                  shape_stream_t *shape_stream,
                  float *weights)
{
    if (!grid || !current_block || !shape_stream || !weights)
        return NULL;

    ensure_cleanup_registered();

    /* Initialize move cache with SEARCH_DEPTH+1 grids for alpha-beta search */
    if (!move_cache_init(SEARCH_DEPTH + 1, grid))
        return NULL;

    /* Perform beam search optimization */
    float best_score;
    move_t *result = search_move_best(grid, shape_stream, weights, &best_score);

    return (best_score == WORST_SCORE) ? NULL : result;
}
