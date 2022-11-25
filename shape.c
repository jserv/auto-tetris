#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nalloc.h"
#include "tetris.h"

static int cmp_coord(const void *a, const void *b)
{
    int *A = *((int **) a), *B = *((int **) b);
    if (A[1] != B[1])
        return -(B[1] - A[1]);
    return A[0] - B[0];
}

static int max_dim(int **coords, int count, int dim)
{
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

static shape_t *shape_new(int **shape_rot)
{
    /* shape_rot is one rotation of the shape */
    shape_t *s = nalloc(sizeof(shape_t), shape_rot);

    /* Normalize to (0, 0) */
    int extreme_left = min_dim(shape_rot, 4, 0);
    int extreme_bot = min_dim(shape_rot, 4, 1);

    /* Define all rotations */
    s->rot[0] = ncalloc(4, sizeof(*s->rot[0]), s);

    /* First rotation: normalize to (0, 0) */
    for (int i = 0; i < 4; i++) {
        s->rot[0][i] = ncalloc(2, sizeof(*s->rot[0][i]), s->rot[0]);
        s->rot[0][i][0] = shape_rot[i][0] - extreme_left;
        s->rot[0][i][1] = shape_rot[i][1] - extreme_bot;
    }
    s->max_dim_len =
        max_ab(max_dim(s->rot[0], 4, 0), max_dim(s->rot[0], 4, 1)) + 1;

    /* Define 1-4 rotations */
    for (int roti = 1; roti < 4; roti++) {
        s->rot[roti] = ncalloc(4, sizeof(*s->rot[roti]), s);
        for (int i = 0; i < 4; i++) {
            s->rot[roti][i] =
                ncalloc(2, sizeof(*s->rot[roti][i]), s->rot[roti]);
            s->rot[roti][i][0] = s->rot[roti - 1][i][1];
            s->rot[roti][i][1] = s->max_dim_len - 1 - s->rot[roti - 1][i][0];
        }

        /* Need to normalize to detect uniqueness later */
        extreme_left = min_dim(s->rot[roti], 4, 0);
        extreme_bot = min_dim(s->rot[roti], 4, 1);
        for (int i = 0; i < 4; i++) {
            s->rot[roti][i][0] -= extreme_left;
            s->rot[roti][i][1] -= extreme_bot;
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
        qsort(s->rot[roti], 4, sizeof(int) * 2, cmp_coord);
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
            s->crust_len[roti][d] = crust_len;
            s->crust[roti][d] = ncalloc(crust_len, sizeof(*s->crust[roti]), s);
            int ii = 0;
            for (int i = 0; i < s->max_dim_len; i++) {
                if (extremes[i][0] != -1) {
                    int index = extremes[i][1];
                    s->crust[roti][d][ii] = ncalloc(
                        2, sizeof(*s->crust[roti][i]), s->crust[roti][d]);
                    s->crust[roti][d][ii][0] = s->rot[roti][index][0];
                    s->crust[roti][d][ii][1] = s->rot[roti][index][1];
                    ii++;
                }
            }
            qsort(s->crust[roti][d], crust_len, sizeof(int) * 2, cmp_coord);
        }
    }

    /* Initialize the flat, more efficient versions */
    for (int r = 0; r < s->n_rot; r++) {
        for (int dim = 0; dim < 2; dim++) {
            for (int i = 0; i < MAX_BLOCK_LEN; i++)
                s->rot_flat[r][i][dim] = s->rot[r][i][dim];
            for (direction_t d = 0; d < 4; d++) {
                for (int i = 0; i < s->crust_len[r][d]; i++)
                    s->crust_flat[d][r][i][dim] = s->crust[d][r][i][dim];
            }
        }
    }
    return s;
}

static shape_t **shapes_read(const char *file, int *count)
{
    FILE *fh = fopen(file, "r");
    if (!fh)
        return NULL;

    *count = 0;
    shape_t **s = nalloc(sizeof(shape_t *), NULL);
    while (!feof(fh)) {
        int **rot = ncalloc(4, sizeof(*rot), s);
        for (int i = 0; i < 4; i++) {
            rot[i] = ncalloc(2, sizeof(*rot[i]), rot);
            if (!fscanf(fh, "%d", &rot[i][0]))
                return NULL;
            if (!fscanf(fh, "%d", &rot[i][1]))
                return NULL;
        }
        s = nrealloc(s, (*count + 1) * sizeof(shape_t *));
        s[(*count)++] = shape_new(rot);
    }

    fclose(fh);

    return s;
}

static int n_shapes;
static shape_t **shapes;

bool shapes_init(char *shapes_file)
{
    return (bool) (shapes = shapes_read(shapes_file, &n_shapes));
}

static inline uint32_t __umulhi(uint32_t a, uint32_t b)
{
    uint64_t c = (uint64_t) a * (uint64_t) b;
    return (uint32_t) (c >> 32);
}

/* Return value in [0,s)
 * See https://lemire.me/blog/2018/12/21/fast-bounded-random-numbers-on-gpus/
 *
 * We should avoid using "rand() % s", which will generate lower numbers more
 * often than higher ones -- it's not a uniform distribution.
 */
static uint32_t ranged_rand(uint32_t s)
{
    uint32_t x = rand();
    uint32_t h = __umulhi(x, s);
    uint32_t l = x * s;
    if (l < s) {
        uint32_t floor = (UINT32_MAX - s + 1) % s;
        while (l < floor) {
            x = rand();
            h = __umulhi(x, s);
            l = x * s;
        }
    }
    return h;
}

/* FIXME: Can we eliminate? */
#define SS_MAX_LEN 3

shape_stream_t *shape_stream_new()
{
    shape_stream_t *s = nalloc(sizeof(*s), NULL);
    s->max_len = SS_MAX_LEN;
    s->iter = 0;
    s->defined = ncalloc(s->max_len, sizeof(*s->defined), s);
    memset(s->defined, false, s->max_len * sizeof(*s->defined));
    s->stream = ncalloc(s->max_len, sizeof(*s->stream), s);
    return s;
}

static shape_t *shape_stream_access(shape_stream_t *stream, int idx)
{
    bool pop = false;
    if (idx == -1) {
        idx = 0;
        pop = true;
    }
    int i = (stream->iter + idx) % stream->max_len;
    if (!stream->defined[i]) {
        stream->stream[i] = shapes[ranged_rand(n_shapes)];
        stream->defined[i] = true;
    }
    if (pop) {
        stream->defined[i] = false;
        stream->iter++;
    }
    return stream->stream[i];
}

shape_t *shape_stream_peek(shape_stream_t *stream, int idx)
{
    return shape_stream_access(stream, idx);
}

shape_t *shape_stream_pop(shape_stream_t *stream)
{
    return shape_stream_access(stream, -1);
}
