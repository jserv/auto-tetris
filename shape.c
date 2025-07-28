#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tetris.h"
#include "utils.h"

/* 7-bag piece generator for fair distribution */
static int bag[7];      /* Holds a shuffled permutation 0-6 */
static int bag_pos = 7; /* 7 = bag empty, needs refill */

static const int base_shapes[][4][2] = {
    /* Square (O-piece)
     * ██
     * ██
     */
    {{0, 0}, {0, 1}, {1, 0}, {1, 1}},

    /* Z-piece
     * ██
     *  ██
     */
    {{0, 1}, {1, 1}, {1, 0}, {2, 1}},

    /* I-piece
     * ████
     */
    {{0, 1}, {1, 1}, {2, 1}, {3, 1}},

    /* L-piece
     * █
     * ███
     */
    {{0, 1}, {1, 1}, {2, 1}, {2, 2}},

    /* J-piece
     *   █
     * ███
     */
    {{0, 1}, {1, 1}, {2, 1}, {2, 0}},

    /* S-piece
     *  ██
     * ██
     */
    {{1, 1}, {2, 1}, {2, 0}, {1, 2}},

    /* T-piece
     * ███
     *  █
     */
    {{1, 1}, {2, 1}, {0, 2}, {1, 2}},
};

#define N_SHAPES (sizeof(base_shapes) / sizeof(base_shapes[0]))

/* Fisher-Yates shuffle of 0..6 */
static void shuffle_bag(void)
{
    for (int i = 0; i < 7; i++)
        bag[i] = i;

    for (int i = 6; i > 0; i--) {
        int j = rand_range(i + 1); /* 0 <= j <= i */
        if (i == j)
            continue;

        int tmp = bag[i];
        bag[i] = bag[j];
        bag[j] = tmp;
    }
    bag_pos = 0;
}

/* Return next piece ID, refilling & shuffling when the bag is empty */
static inline int bag_next(void)
{
    if (bag_pos >= 7)
        shuffle_bag();
    return bag[bag_pos++];
}

/* Reset bag state for testing purposes */
void shape_bag_reset(void)
{
    bag_pos = 7; /* Force refill on next bag_next() call */
}

static inline void sort_coords(int **coords, int count, int max_dim_len)
{
    /* Bit-twiddling coordinate sort: map each (x,y) to a bit position,
     * then use compiler builtins to traverse in sorted order. */
    int *pos_ptr[16] = {0};
    unsigned mask = 0;

    for (int i = 0; i < count; i++) {
        int pos = coords[i][1] * max_dim_len + coords[i][0];
        pos_ptr[pos] = coords[i];
        mask |= 1u << pos;
    }

    int idx = 0;
    for (int y = max_dim_len - 1; y >= 0 && mask; y--) {
        unsigned row_mask =
            (mask >> (y * max_dim_len)) & ((1u << max_dim_len) - 1);

        while (row_mask) {
            int x = CTZ(row_mask); /* lowest x first */
            int pos = y * max_dim_len + x;
            coords[idx++] = pos_ptr[pos];

            row_mask &= row_mask - 1; /* clear the lowest set bit */
            mask &= ~(1u << pos);     /* mirror clear in the global mask */
        }
    }
}

static int max_dim(int **coords, int count, int dim)
{
    if (count <= 0)
        return 0;

    int mx = coords[0][dim];
    for (int i = 1; i < count; i++) {
        int curr = coords[i][dim];
        if (curr > mx)
            mx = curr;
    }
    return mx;
}

static int min_dim(int **coords, int count, int dim)
{
    if (count <= 0)
        return 0;

    int mn = coords[0][dim];
    for (int i = 1; i < count; i++) {
        int curr = coords[i][dim];
        if (curr < mn)
            mn = curr;
    }
    return mn;
}

static inline int max_ab(int a, int b)
{
    return a > b ? a : b;
}

/* Compute geometry signature for shape */
static unsigned compute_shape_sig(const shape_t *s)
{
    if (!s)
        return 0;

    unsigned sig = 0;

    /* Use normalized first rotation for consistent signature */
    for (int i = 0; i < MAX_BLOCK_LEN; i++) {
        int x = s->rot_flat[0][i][0];
        int y = s->rot_flat[0][i][1];

        /* Skip invalid coordinates */
        if (x < 0 || y < 0 || x >= 4 || y >= 4)
            continue;

        sig |= 1u << (y * 4 + x); /* Set bit for each occupied cell */
    }
    return sig;
}

static shape_t *shape_new(int **shape_rot)
{
    if (!shape_rot)
        return NULL;

    /* shape_rot is one rotation of the shape */
    shape_t *s = nalloc(sizeof(shape_t), shape_rot);
    if (!s)
        return NULL;

    /* Normalize to (0, 0) */
    int left = min_dim(shape_rot, 4, 0);
    int bot = min_dim(shape_rot, 4, 1);

    /* Define all rotations */
    s->rot[0] = ncalloc(4, sizeof(*s->rot[0]), s);
    if (!s->rot[0]) {
        nfree(s);
        return NULL;
    }

    /* First rotation: normalize to (0, 0) */
    for (int i = 0; i < 4; i++) {
        s->rot[0][i] = ncalloc(2, sizeof(*s->rot[0][i]), s->rot[0]);
        if (!s->rot[0][i]) {
            nfree(s);
            return NULL;
        }
        s->rot[0][i][0] = shape_rot[i][0] - left;
        s->rot[0][i][1] = shape_rot[i][1] - bot;
    }
    s->max_dim_len =
        max_ab(max_dim(s->rot[0], 4, 0), max_dim(s->rot[0], 4, 1)) + 1;

    /* Define 1-4 rotations */
    for (int roti = 1; roti < 4; roti++) {
        s->rot[roti] = ncalloc(4, sizeof(*s->rot[roti]), s);
        if (!s->rot[roti]) {
            nfree(s);
            return NULL;
        }

        for (int i = 0; i < 4; i++) {
            s->rot[roti][i] =
                ncalloc(2, sizeof(*s->rot[roti][i]), s->rot[roti]);
            if (!s->rot[roti][i]) {
                nfree(s);
                return NULL;
            }
            s->rot[roti][i][0] = s->rot[roti - 1][i][1];
            s->rot[roti][i][1] = s->max_dim_len - 1 - s->rot[roti - 1][i][0];
        }

        /* Need to normalize to detect uniqueness later */
        left = min_dim(s->rot[roti], 4, 0);
        bot = min_dim(s->rot[roti], 4, 1);
        for (int i = 0; i < 4; i++) {
            s->rot[roti][i][0] -= left;
            s->rot[roti][i][1] -= bot;
        }
    }

    /* Initialize s->rot_wh */
    for (int roti = 0; roti < 4; roti++) {
        s->rot_wh[roti].x = max_dim(s->rot[roti], 4, 0) + 1;
        s->rot_wh[roti].y = max_dim(s->rot[roti], 4, 1) + 1;
    }

    /* Determine number of unique rotations */
    char rot_str[4][4 * 2 + 1];
    s->n_rot = 0;
    for (int roti = 0; roti < 4; roti++) {
        rot_str[roti][4 * 2] = '\0';
        sort_coords(s->rot[roti], 4, s->max_dim_len);
        for (int i = 0; i < 4; i++) {
            rot_str[roti][2 * i] = '0' + s->rot[roti][i][0];
            rot_str[roti][2 * i + 1] = '0' + s->rot[roti][i][1];
        }
        for (int i = 0; i < roti; i++) {
            if (!strcmp(rot_str[i], rot_str[roti]))
                goto setup;
        }
        s->n_rot++;
    }

setup:
    /* Define crusts */
    for (int roti = 0; roti < 4; roti++) {
        for (direction_t d = 0; d < 4; d++) {
            int extremes[s->max_dim_len][2];  // value, index
            int dim = (d == BOT || d == TOP) ? 1 : 0;
            bool keep_max = (d == TOP || d == RIGHT);

            for (int i = 0; i < s->max_dim_len; i++)
                extremes[i][0] = -1;

            int crust_len = 0;
            for (int i = 0; i < 4; i++) {
                int key = s->rot[roti][i][(dim + 1) % 2];
                int val = s->rot[roti][i][dim];

                if (key >= 0 && key < s->max_dim_len) {
                    int curr = extremes[key][0];
                    bool replace = curr == -1 || (keep_max && val > curr) ||
                                   (!keep_max && val < curr);
                    if (curr == -1)
                        crust_len++;

                    if (replace) {
                        extremes[key][0] = val;
                        extremes[key][1] = i;
                    }
                }
            }
            s->crust_len[roti][d] = crust_len;
            s->crust[roti][d] = ncalloc(crust_len, sizeof(*s->crust[roti]), s);
            if (!s->crust[roti][d] && crust_len > 0) {
                nfree(s);
                return NULL;
            }

            int ii = 0;
            for (int i = 0; i < s->max_dim_len && ii < crust_len; i++) {
                if (extremes[i][0] != -1) {
                    int index = extremes[i][1];
                    s->crust[roti][d][ii] = ncalloc(
                        2, sizeof(*s->crust[roti][d][ii]), s->crust[roti][d]);
                    if (!s->crust[roti][d][ii]) {
                        nfree(s);
                        return NULL;
                    }
                    s->crust[roti][d][ii][0] = s->rot[roti][index][0];
                    s->crust[roti][d][ii][1] = s->rot[roti][index][1];
                    ii++;
                }
            }

            if (crust_len > 0)
                sort_coords(s->crust[roti][d], crust_len, s->max_dim_len);
        }
    }

    /* Initialize the flat, more efficient versions */
    for (int r = 0; r < s->n_rot && r < 4; r++) {
        for (int dim = 0; dim < 2; dim++) {
            for (int i = 0; i < MAX_BLOCK_LEN; i++)
                s->rot_flat[r][i][dim] = s->rot[r][i][dim];
            for (direction_t d = 0; d < 4; d++) {
                int len = s->crust_len[r][d];
                for (int i = 0; i < len && i < MAX_BLOCK_LEN; i++)
                    s->crust_flat[r][d][i][dim] = s->crust[r][d][i][dim];
            }
        }
    }

    /* Compute and store geometry signature for color lookup optimization */
    s->sig = compute_shape_sig(s);

    return s;
}

static int n_shapes;
static shape_t **shapes;

bool shape_init(void)
{
    shapes = nalloc(N_SHAPES * sizeof(shape_t *), NULL);
    if (!shapes)
        return false;

    /* Clear shapes array first */
    for (int i = 0; i < N_SHAPES; i++)
        shapes[i] = NULL;

    n_shapes = 0;

    /* Try to create all shapes */
    for (int idx = 0; idx < N_SHAPES; idx++) {
        /* Create rotation data structure compatible with shape_new */
        int **rot = ncalloc(4, sizeof(*rot), shapes);
        if (!rot) {
            /* Cleanup and fail completely */
            for (int j = 0; j < n_shapes; j++)
                nfree(shapes[j]);
            nfree(shapes);
            shapes = NULL;
            n_shapes = 0;
            return false;
        }

        bool rot_valid = true;
        for (int i = 0; i < 4; i++) {
            rot[i] = ncalloc(2, sizeof(*rot[i]), rot);
            if (!rot[i]) {
                rot_valid = false;
                break;
            }
            rot[i][0] = base_shapes[idx][i][0];
            rot[i][1] = base_shapes[idx][i][1];
        }

        if (!rot_valid) {
            nfree(rot);
            /* Cleanup and fail completely */
            for (int j = 0; j < n_shapes; j++)
                nfree(shapes[j]);
            nfree(shapes);
            shapes = NULL;
            n_shapes = 0;
            return false;
        }

        shape_t *new_shape = shape_new(rot);
        if (!new_shape) {
            nfree(rot);
            /* Cleanup and fail completely */
            for (int j = 0; j < n_shapes; j++)
                nfree(shapes[j]);
            nfree(shapes);
            shapes = NULL;
            n_shapes = 0;
            return false;
        }

        shapes[n_shapes++] = new_shape;
    }

    /* Verify we got exactly the expected number of shapes */
    if (n_shapes != N_SHAPES) {
        /* This should never happen with the fixed logic, but safety check */
        for (int j = 0; j < n_shapes; j++)
            nfree(shapes[j]);
        nfree(shapes);
        shapes = NULL;
        n_shapes = 0;
        return false;
    }

    return true;
}

/* Return a shape by index (for falling pieces effect) */
shape_t *shape_get(int index)
{
    if (index < 0 || index >= NUM_TETRIS_SHAPES || !shapes ||
        n_shapes != NUM_TETRIS_SHAPES)
        return NULL;
    return shapes[index];
}

/* FIXME: Can we eliminate? */
#define SS_MAX_LEN 3

shape_stream_t *shape_stream_new()
{
    shape_stream_t *s = nalloc(sizeof(*s), NULL);
    if (!s)
        return NULL;

    s->max_len = SS_MAX_LEN;
    s->iter = 0;
    s->defined = ncalloc(s->max_len, sizeof(*s->defined), s);
    s->stream = ncalloc(s->max_len, sizeof(*s->stream), s);

    if (!s->defined || !s->stream) {
        nfree(s);
        return NULL;
    }

    memset(s->defined, false, s->max_len * sizeof(*s->defined));
    return s;
}

static shape_t *stream_access(shape_stream_t *stream, int idx)
{
    if (!stream || n_shapes != NUM_TETRIS_SHAPES || !shapes)
        return NULL;

    bool pop = false;
    if (idx == -1) {
        idx = 0;
        pop = true;
    }

    if (idx < 0 || idx >= stream->max_len)
        return NULL;

    int i = (stream->iter + idx) % stream->max_len;
    if (!stream->defined[i]) {
        /* Use the 7-bag to guarantee one of each tetromino every 7 deals */
        int shape_idx = bag_next(); /* values 0-6 */
        /* Validate that we have all shapes available */
        if (shape_idx >= n_shapes || shape_idx >= NUM_TETRIS_SHAPES) {
            /* This indicates a serious bug - bag should only return 0-6, and we
             * should have exactly 7 shapes. Return NULL to fail gracefully.
             */
            return NULL;
        }
        stream->stream[i] = shapes[shape_idx];
        stream->defined[i] = true;
    }

    if (pop) {
        stream->defined[i] = false;
        stream->iter++;
    }

    return stream->stream[i];
}

shape_t *shape_stream_peek(const shape_stream_t *stream, int idx)
{
    return stream_access((shape_stream_t *) stream, idx);
}

shape_t *shape_stream_pop(shape_stream_t *stream)
{
    return stream_access(stream, -1);
}

void shape_free(void)
{
    if (shapes) {
        nfree(shapes);
        shapes = NULL;
    }
    n_shapes = 0;
}
