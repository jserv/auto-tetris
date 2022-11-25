#include <float.h>
#include <stdlib.h>
#include <string.h>

#include "nalloc.h"
#include "tetris.h"

#define MOST_NEG_FLOAT (-FLT_MAX)

#define MAX(a, b) ((a) > (b) ? (a) : (b))

enum {
    FEATIDX_RELIEF_MAX = 0,
    FEATIDX_RELIEF_AVG,
    FEATIDX_RELIEF_VAR,
    FEATIDX_GAPS,
    FEATIDX_OBS,
    FEATIDX_DISCONT,
    N_FEATIDX,
};

/* FIXME: allow future adjustments */
static const float predefined_weights[] = {
    [FEATIDX_RELIEF_MAX] = 0.23,  [FEATIDX_RELIEF_AVG] = -3.62,
    [FEATIDX_RELIEF_VAR] = -0.21, [FEATIDX_GAPS] = -0.89,
    [FEATIDX_OBS] = -0.96,        [FEATIDX_DISCONT] = -0.27,
};

float *default_weights()
{
    float *w = malloc(sizeof(predefined_weights));
    memcpy(w, predefined_weights, sizeof(predefined_weights));
    return w;
}

static void feature_variance(const grid_t *g, float *raws)
{
    float avg = 0, var = 0, max = 0;
    int width = g->width;

    int discont = -1, last_height = -1;
    int gaps = 0, obs = 0;
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

    for (int i = 0; i < width; i++) {
        float diff = avg - g->relief[i];
        var += diff * diff;
    }

    raws[FEATIDX_RELIEF_MAX] = max;
    raws[FEATIDX_RELIEF_AVG] = avg;
    raws[FEATIDX_RELIEF_VAR] = var;
    raws[FEATIDX_DISCONT] = discont;
    raws[FEATIDX_GAPS] = gaps;
    raws[FEATIDX_OBS] = obs;
}

static float grid_eval(grid_t *g, const float *weights)
{
    float raws[N_FEATIDX];
    feature_variance(g, raws);
    float val = 0;
    for (int i = 0; i < N_FEATIDX; i++)
        val += raws[i] * weights[i];
    return val;
}

static grid_t **grids = NULL;
static block_t **blocks = NULL;
static move_t **best_moves = NULL;

static move_t *best_move_rec(grid_t *g,
                             shape_stream_t *stream,
                             float *w,
                             int depth_left,
                             float *value,
                             int relief_max)
{
    float score = MOST_NEG_FLOAT;

    int depth = stream->max_len - depth_left - 1;
    shape_t *s = shape_stream_peek(stream, depth);
    block_t *b = blocks[depth_left];
    move_t *best = best_moves[depth_left];

    /* In cases when we need to clear lines */
    grid_t *g_prime = grids[depth_left]; /* depth_left: 0...ss->max_len-1 */

    best->shape = s;
    int max_rots = s->n_rot;
    b->shape = s;
    bool nocheck = (g->height - 1 - relief_max) >= b->shape->max_dim_len;
    int elevated = g->height - b->shape->max_dim_len;

    /* FIXME: it should be taken from grid_block_elevate */
    b->offset.y = elevated;

    for (int r = 0; r < max_rots; r++) {
        b->rot = r;
        int max_cols = g->width - s->rot_wh[r].x + 1;
        b->offset.y = elevated;
        int top_elevated = block_extreme(b, TOP);
        for (int c = 0; c < max_cols; c++) {
            b->offset.x = c;
            b->offset.y = elevated;
            if (!nocheck && grid_block_intersects(g, b))
                continue;

            int amt = grid_block_drop(g, b);
            grid_block_add(g, b);
            int new_relief_mx = MAX(relief_max, top_elevated - amt);

            grid_t *g_rec;
            if (g->n_full_rows) {
                g_rec = g_prime;
                grid_cpy(g_rec, g);
                grid_clear_lines(g_rec);
            } else {
                g_rec = g;
            }

            float curr;
            if (depth_left) {
                best_move_rec(g_rec, stream, w, depth_left - 1, &curr,
                              new_relief_mx);
            } else {
                curr = grid_eval(g_rec, w);
            }
            if (curr > score) {
                score = curr;
                best->rot = r;
                best->col = c;
            }

            grid_block_remove(g, b);
        }
    }
    *value = score;
    return best;
}

static int n_grids = 0;

move_t *best_move(grid_t *g, block_t *b, shape_stream_t *ss, float *w)
{
    if (n_grids < ss->max_len) {
        int depth = ss->max_len;
        grids = ncalloc(depth, sizeof(*grids), g);
        blocks = ncalloc(depth, sizeof(*blocks), b);
        best_moves = nrealloc(best_moves, depth * sizeof(*best_moves));
        for (int i = n_grids; i < ss->max_len; i++) {
            grids[i] = grid_new(g->height, g->width);
            nalloc_set_parent(grids[i], grids);
            blocks[i] = block_new();
            nalloc_set_parent(blocks[i], blocks);
            best_moves[i] = nalloc(sizeof(*best_moves[i]), best_moves);
        }
        n_grids = ss->max_len;
    }

    int relief_mx = -1;
    for (int i = 0; i < g->width; i++)
        relief_mx = MAX(relief_mx, g->relief[i]);

    float val;
    move_t *best = best_move_rec(g, ss, w, ss->max_len - 1, &val, relief_mx);
    return val == MOST_NEG_FLOAT ? NULL : best;
}
