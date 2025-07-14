#include <float.h>
#include <stdlib.h>
#include <string.h>

#include "nalloc.h"
#include "tetris.h"

#define WORST_SCORE (-FLT_MAX)
#define MAX(a, b) ((a) > (b) ? (a) : (b))

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
        obs += height - cgaps;
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

/* Evaluate grid position using weighted features */
static float evaluate_grid(grid_t *g, const float *weights)
{
    if (!g || !weights)
        return WORST_SCORE;

    float features[N_FEATIDX];
    calculate_features(g, features);

    /* Calculate weighted score */
    float score = 0.0f;
    for (int i = 0; i < N_FEATIDX; i++)
        score += features[i] * weights[i];

    return score;
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
}

/* Register cleanup function */
void move_cleanup_atexit(void)
{
    atexit(move_cache_cleanup);
}

/* Single-level search for best move */
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

    /* Try all possible placements for current piece only */
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

            if (current_grid->n_full_rows > 0) {
                /* Copy grid and clear lines */
                grid_for_evaluation = evaluation_grid;
                grid_cpy(grid_for_evaluation, current_grid);
                grid_clear_lines(grid_for_evaluation);
            } else {
                grid_for_evaluation = current_grid;
            }

            /* Evaluate final position directly */
            float position_score = evaluate_grid(grid_for_evaluation, weights);

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

    /* Initialize move cache if needed */
    if (!move_cache_init(1, grid)) /* Only need depth 1 */
        return NULL;

    /* Find maximum relief height for optimization */
    int max_relief = -1;
    for (int i = 0; i < grid->width; i++)
        max_relief = MAX(max_relief, grid->relief[i]);

    /* Perform non-recursive search */
    float best_score;
    move_t *result =
        search_best_move(grid, shape_stream, weights, &best_score, max_relief);

    return (best_score == WORST_SCORE) ? NULL : result;
}
