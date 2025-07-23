#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tetris.h"
#include "utils.h"

/* Zobrist hashing system for incremental grid hash computation */

/* Zobrist table: each cell (x,y) has a unique 64-bit random number */
static uint64_t ztable[GRID_WIDTH][GRID_HEIGHT];

/* Boundary checking helper with unsigned comparison optimization */
static inline bool in_bounds(const grid_t *g, int x, int y)
{
    return g && (unsigned) x < (unsigned) g->width &&
           (unsigned) y < (unsigned) g->height;
}

/* Initialize Zobrist table with high-quality random numbers */
void grid_init(void)
{
    /* Seed with current time for variety across runs */
    uint64_t seed = (uint64_t) time(NULL) ^ (uint64_t) grid_init; /* ASLR */

    /* Use xorshift64* PRNG for high-quality pseudo-random numbers */
    for (int x = 0; x < GRID_WIDTH; x++) {
        for (int y = 0; y < GRID_HEIGHT; y++) {
            /* xorshift64* algorithm */
            seed ^= seed >> 12;
            seed ^= seed << 25;
            seed ^= seed >> 27;
            ztable[x][y] = seed * 0x2545F4914F6CDD1DULL;
        }
    }
}

static void grid_reset(grid_t *g)
{
    if (!g)
        return;

    for (int r = 0; r < g->height; r++)
        memset(g->rows[r], 0, g->width * sizeof(*g->rows[r]));

    for (int c = 0; c < g->width; c++) {
        g->relief[c] = -1;
        g->gaps[c] = 0;
        g->stack_cnt[c] = 0;
    }

    memset(g->n_row_fill, 0, g->height * sizeof(*g->n_row_fill));
    g->n_total_cleared = 0;
    g->n_last_cleared = 0;
    g->n_full_rows = 0;
    g->hash = 0; /* Reset Zobrist hash */
}

grid_t *grid_new(int height, int width)
{
    if (height <= 0 || width <= 0)
        return NULL;

    grid_t *g = nalloc(sizeof(grid_t), NULL);
    if (!g)
        return NULL;

    g->width = width, g->height = height;
    g->rows = ncalloc(height, sizeof(*g->rows), g);
    g->stacks = ncalloc(width, sizeof(*g->stacks), g);
    g->relief = ncalloc(width, sizeof(*g->relief), g);
    g->gaps = ncalloc(width, sizeof(*g->gaps), g);
    g->stack_cnt = ncalloc(width, sizeof(*g->stack_cnt), g);
    g->n_row_fill = ncalloc(height, sizeof(*g->n_row_fill), g);
    g->full_rows = ncalloc(height, sizeof(*g->full_rows), g);

    if (!g->rows || !g->stacks || !g->relief || !g->gaps || !g->stack_cnt ||
        !g->n_row_fill || !g->full_rows) {
        nfree(g);
        return NULL;
    }

    for (int r = 0; r < g->height; r++) {
        g->rows[r] = ncalloc(g->width, sizeof(*g->rows[r]), g);
        if (!g->rows[r]) {
            nfree(g);
            return NULL;
        }
    }

    for (int c = 0; c < g->width; c++) {
        g->stacks[c] = ncalloc(g->height, sizeof(*g->stacks[c]), g);
        if (!g->stacks[c]) {
            nfree(g);
            return NULL;
        }
    }

    grid_reset(g);
    return g;
}

void grid_copy(grid_t *dst, const grid_t *src)
{
    if (!dst || !src || dst->height != src->height ||
        dst->width != src->width) {
        return;
    }

    dst->n_full_rows = src->n_full_rows;
    dst->width = src->width, dst->height = src->height;
    dst->n_last_cleared = src->n_last_cleared;
    dst->n_total_cleared = src->n_total_cleared;
    dst->hash = src->hash; /* Copy Zobrist hash */

    for (int i = 0; i < src->height; i++)
        memcpy(dst->rows[i], src->rows[i], src->width * sizeof(*src->rows[i]));

    for (int i = 0; i < src->width; i++)
        memcpy(dst->stacks[i], src->stacks[i],
               src->height * sizeof(*src->stacks[i]));

    memcpy(dst->full_rows, src->full_rows,
           src->height * sizeof(*src->full_rows));
    memcpy(dst->n_row_fill, src->n_row_fill,
           src->height * sizeof(*src->n_row_fill));
    memcpy(dst->relief, src->relief, src->width * sizeof(*src->relief));
    memcpy(dst->stack_cnt, src->stack_cnt,
           src->width * sizeof(*src->stack_cnt));
    memcpy(dst->gaps, src->gaps, src->width * sizeof(*src->gaps));
}

static inline int height_at(const grid_t *g, int x, int start_at)
{
    if (!g || !in_bounds(g, x, start_at))
        return -1;

    int y;
    for (y = start_at; y >= 0 && !g->rows[y][x]; y--)
        ;
    return y;
}

static void remove_full_row(grid_t *g, int r)
{
    if (!g || g->n_full_rows <= 0)
        return;

    int last_full_idx = g->n_full_rows - 1;
    if (g->full_rows[last_full_idx] != r) {
        int i;
        for (i = 0; i < g->n_full_rows && g->full_rows[i] != r; i++)
            ;
        if (i < g->n_full_rows)
            g->full_rows[i] = g->full_rows[last_full_idx];
    }
    g->n_full_rows--;
}

static void cell_add(grid_t *g, int r, int c)
{
    if (!in_bounds(g, c, r))
        return;

    g->rows[r][c] = true;
    g->hash ^= ztable[c][r]; /* Update Zobrist hash */

    /* Increment fill count and immediately check for full row, avoiding the
     * need to scan the entire row later.
     */
    if (++g->n_row_fill[r] == g->width && g->n_full_rows < g->height)
        g->full_rows[g->n_full_rows++] = r;

    int top = g->relief[c];
    if (top < r) {
        g->relief[c] = r;
        g->gaps[c] += r - 1 - top;
        if (g->stack_cnt[c] < g->height)
            g->stacks[c][g->stack_cnt[c]++] = r;
    } else {
        g->gaps[c]--;
        /* adding under the relief */
        int idx = g->stack_cnt[c] - 1; /* insert idx */
        for (; idx > 0 && g->stacks[c][idx - 1] > r; idx--)
            ;
        if (g->stack_cnt[c] < g->height) {
            memmove(g->stacks[c] + idx + 1, g->stacks[c] + idx,
                    (g->stack_cnt[c] - idx) * sizeof(*g->stacks[c]));
            g->stacks[c][idx] = r;
            g->stack_cnt[c]++;
        }
    }
}

/* Optimized cell removal with efficient full-row list maintenance */
static void cell_remove(grid_t *g, int r, int c)
{
    if (!in_bounds(g, c, r))
        return;

    g->rows[r][c] = false;
    g->hash ^= ztable[c][r]; /* Update Zobrist hash */

    /* Check if row was full before decrementing count
     * This maintains the full_rows list efficiently */
    if (g->n_row_fill[r] == g->width) {
        /* Row was full, now it won't be - remove from full_rows list */
        remove_full_row(g, r);
    }
    g->n_row_fill[r]--;

    int top = g->relief[c];
    if (top == r) {
        if (g->stack_cnt[c] > 0) {
            g->stack_cnt[c]--;
            int new_top =
                g->stack_cnt[c] ? g->stacks[c][g->stack_cnt[c] - 1] : -1;
            g->relief[c] = new_top;
            g->gaps[c] -= (top - 1 - new_top);
        }
    } else {
        g->gaps[c]++;

        /* removing under the relief */
        int idx = g->stack_cnt[c] - 1;
        for (; idx >= 0 && g->stacks[c][idx] != r; idx--)
            ;
        if (idx >= 0 && g->stack_cnt[c] > 0) {
            memmove(g->stacks[c] + idx, g->stacks[c] + idx + 1,
                    (g->stack_cnt[c] - idx - 1) * sizeof(*g->stacks[c]));
            g->stack_cnt[c]--;
        }
    }
}

void grid_block_add(grid_t *g, const block_t *b)
{
    if (!g || !b || !b->shape)
        return;

    int dc = b->offset.x, dr = b->offset.y;
    int *rot = (int *) b->shape->rot_flat[b->rot];

    for (int i = 0; i < 2 * 4; i += 2) {
        int c = rot[i] + dc;
        int r = rot[i + 1] + dr;
        cell_add(g, r, c);
    }
}

void grid_block_remove(grid_t *g, const block_t *b)
{
    if (!g || !b || !b->shape)
        return;

    int dc = b->offset.x, dr = b->offset.y;
    int *rot = (int *) (b->shape->rot_flat[b->rot]);

    for (int i = 2 * 3; i >= 0; i -= 2) {
        int c = rot[i] + dc;
        int r = rot[i + 1] + dr;
        cell_remove(g, r, c);
    }
}

static int max_h(const int *heights, int count)
{
    if (count <= 0)
        return 0;

    int mx = heights[0];
    for (int i = 1; i < count; i++) {
        int curr = heights[i];
        mx = curr > mx ? curr : mx;
    }
    return mx;
}

/* Order the full‑row list in descending (top‑to‑bottom)
 * row index so grid_clear_lines() can pop from the tail.
 */
static int cmp_row_desc(const void *a, const void *b)
{
    int ra = *(const int *) a;
    int rb = *(const int *) b;
    /* Descending: return <0 when rb < ra (i.e. ra comes *before* rb) */
    return (rb - ra);
}

static void sort_rows(int *full_rows, int count)
{
    if (count > 1)
        qsort(full_rows, (size_t) count, sizeof(*full_rows), cmp_row_desc);
}

int grid_clear_lines(grid_t *g)
{
    if (!g || !g->n_full_rows)
        return 0;

    int expected_count = g->n_full_rows;
    int cleared_count = 0;
    bool **cleared = calloc(expected_count, sizeof(bool *));
    if (!cleared)
        return 0;

    /* Smaller values means near bottom of the grid. i.e., descending order.
     * Therefore,  we can just decrement the count to "pop" the smallest row.
     */
    sort_rows(g->full_rows, g->n_full_rows);

    /* Smallest full row */
    int y = g->full_rows[g->n_full_rows - 1];

    /* Largest occupied (full or non-full) row */
    int ymax = max_h(g->relief, g->width);

    int next_non_full = y + 1;
    while (next_non_full <= ymax && y < g->height) {
        /* Copy next non-full row into y, which is either full or has already
         * been copied into a lower y.
         * if it is full, we zero it and save it for the end.
         */

        /* find the next non-full - use n_row_fill for O(1) check */
        while (next_non_full <= ymax && next_non_full < g->height &&
               g->n_row_fill[next_non_full] == g->width) {
            next_non_full++;
        }

        /* There is no next non full to copy into a row below */
        if (next_non_full > ymax || next_non_full >= g->height)
            break;

        if (g->n_row_fill[y] == g->width) {
            /* in this case, save row y for the end */
            if (g->n_full_rows > 0) {
                g->n_full_rows--;
                if (cleared_count < expected_count)
                    cleared[cleared_count++] = g->rows[y];

                /* Update Zobrist hash: remove all cells from old row y */
                for (int x = 0; x < g->width; x++)
                    g->hash ^= ztable[x][y];
            }
        }

        /* Reuse the row, no need to allocate new memory.
         * copy next-non-full into y, which was previously a next-non-full
         * and already copied, or y is full and we saved it.
         */
        if (next_non_full < g->height) {
            /* Update Zobrist hash: account for row movement */
            for (int x = 0; x < g->width; x++) {
                if (g->rows[next_non_full][x]) {
                    /* remove from old position */
                    g->hash ^= ztable[x][next_non_full];
                    g->hash ^= ztable[x][y]; /* add to new position */
                }
            }

            g->rows[y] = g->rows[next_non_full];
            g->n_row_fill[y] = g->n_row_fill[next_non_full];
        }

        y++;
        next_non_full++;
    }

    /* There might be left-over rows that were cleared.
     * they need to be zeroed-out, and replaces into rows[y...ymax]
     */
    g->n_total_cleared += expected_count;
    g->n_last_cleared = expected_count;

    while (y < g->height && (cleared_count > 0 || g->n_full_rows > 0)) {
        if (g->n_full_rows > 0) {
            /* Update Zobrist hash: remove all cells from the full row */
            int full_row_idx = g->full_rows[--g->n_full_rows];
            for (int x = 0; x < g->width; x++) {
                if (g->rows[full_row_idx][x])
                    g->hash ^= ztable[x][full_row_idx];
            }

            g->rows[y] = g->rows[full_row_idx];
        } else if (cleared_count > 0) {
            g->rows[y] = cleared[--cleared_count];
        }
        g->n_row_fill[y] = 0;
        if (g->rows[y])
            memset(g->rows[y], 0, g->width * sizeof(*g->rows[y]));
        y++;
    }

    /* We need to update relief and stacks */
    for (int i = 0; i < g->width; i++) {
        int new_top = height_at(g, i, g->relief[i]);
        g->relief[i] = new_top;
        int gaps = 0;
        g->stack_cnt[i] = 0;
        for (int ii = 0; ii <= new_top && ii < g->height; ii++) {
            if (g->rows[ii][i]) {
                if (g->stack_cnt[i] < g->height) {
                    g->stacks[i][g->stack_cnt[i]++] = ii;
                }
            } else {
                gaps++;
            }
        }
        g->gaps[i] = gaps;
    }

    free(cleared);
    return g->n_last_cleared;
}

/* Consolidated bounds checking function */
static bool block_in_bounds(const grid_t *g, const block_t *b)
{
    if (!g || !b || !b->shape)
        return false;

    /* Check all block coordinates are within grid bounds */
    for (int i = 0; i < MAX_BLOCK_LEN; i++) {
        coord_t cr;
        block_get(b, i, &cr);
        if (!in_bounds(g, cr.x, cr.y))
            return false;
    }
    return true;
}

bool grid_block_collides(const grid_t *g, const block_t *b)
{
    if (!g || !b || !b->shape)
        return true;

    /* Fast path: use flattened coordinates and early bounds check */
    const shape_t *s = b->shape;
    int sx = b->offset.x, sy = b->offset.y;

    /* Early bounds check using precomputed shape dimensions */
    if (sx < 0 || sy < 0 || sx + s->rot_wh[b->rot].x > g->width ||
        sy + s->rot_wh[b->rot].y > g->height)
        return true;

    /* Use flattened coordinates for better cache performance */
    const int (*coords)[2] = (const int (*)[2]) s->rot_flat[b->rot];

    for (int i = 0; i < MAX_BLOCK_LEN; i++) {
        int x = coords[i][0], y = coords[i][1];

        /* Skip invalid coordinates (marked as negative) */
        if (x < 0 || y < 0)
            continue;

        int gx = sx + x, gy = sy + y;

        /* Check collision with occupied cells */
        if (g->rows[gy][gx])
            return true;
    }

    return false;
}

/* Consolidated block validation function */
static inline bool block_valid(const grid_t *g, const block_t *b)
{
    return block_in_bounds(g, b) && !grid_block_collides(g, b);
}

static inline int block_elevate(const grid_t *g, block_t *b)
{
    if (!g || !b || !b->shape)
        return 0;

    /* offset.y needs to be in-bounds for all rotations, so
     * extreme(b, TOP) == 0 will not always be the case.
     */
    b->offset.y = g->height - b->shape->max_dim_len;

    /* In-bounds check should never fail here for legal, known shapes.
     * It is a function of the grid dimensions and shape structure only.
     * This property can be checked once for each shape.
     */
    return !grid_block_collides(g, b);
}

int grid_block_spawn(const grid_t *g, block_t *b)
{
    if (!g || !b || !b->shape)
        return 0;

    /*Return whether block was successfully centered */
    b->offset.x = (g->width - b->shape->rot_wh[b->rot].x) / 2;
    return block_elevate(g, b);
}

static int drop_amount(const grid_t *g, const block_t *b)
{
    if (!g || !b || !b->shape)
        return 0;

    int min_amnt = INT_MAX;
    int dc = b->offset.x, dr = b->offset.y;
    int rot = b->rot;
    int crust_len = b->shape->crust_len[rot][BOT];
    int *crust = (int *) (b->shape->crust_flat[rot][BOT]);

    for (int i = 0; i < crust_len; i++) {
        int c = *crust++ + dc;
        int r = *crust++ + dr;

        if (in_bounds(g, c, 0)) {
            int amnt = r - (g->relief[c] + 1);
            if (amnt < min_amnt)
                min_amnt = amnt;
        }
    }

    if (min_amnt >= 0)
        goto back;

    /* relief can not help us, as we are under the relief */
    min_amnt = 0;
    int max_amnt = block_extreme(b, BOT);
    for (min_amnt = 0; min_amnt < max_amnt; min_amnt++) {
        int next_amnt = min_amnt + 1;
        for (int i = 0; i < b->shape->crust_len[b->rot][BOT]; i++) {
            int *cr = b->shape->crust[b->rot][BOT][i];
            int c = cr[0] + b->offset.x, r = cr[1] + b->offset.y;
            if (in_bounds(g, c, r - next_amnt) && g->rows[r - next_amnt][c])
                goto back;
        }
    }

back:
    return min_amnt < 0 ? 0 : min_amnt;
}

int grid_block_drop(const grid_t *g, block_t *b)
{
    if (!g || !b)
        return 0;

    int amount = drop_amount(g, b);
    block_move(b, BOT, amount);
    return amount;
}

/* Consolidated movement function with consistent validation */
void grid_block_move(const grid_t *g, block_t *b, direction_t d, int amount)
{
    if (!g || !b || !b->shape)
        return;

    block_move(b, d, amount);
    if (!block_valid(g, b))
        block_move(b, d, -amount);
}

/* Consolidated rotation function with consistent validation */
void grid_block_rotate(const grid_t *g, block_t *b, int amount)
{
    if (!g || !b || !b->shape)
        return;

    block_rotate(b, amount);
    if (!block_valid(g, b))
        block_rotate(b, -amount);
}

bool grid_is_tetris_ready(const grid_t *g, int *well_col)
{
    if (!g || !g->relief || !well_col) {
        if (well_col)
            *well_col = -1;
        return false;
    }

    *well_col = -1;

    /* Minimum well depth for Tetris opportunity */
    const int MIN_WELL_DEPTH = 4;

    /* Check each column to see if it forms a suitable well */
    for (int x = 0; x < g->width; x++) {
        int well_height = g->relief[x] + 1; /* current height of this column */

        /* Check if this column is significantly lower than neighbors */
        int min_neighbor_height = g->height; /* start with max possible */

        /* Check left neighbor */
        if (x > 0) {
            int left_height = g->relief[x - 1] + 1;
            if (left_height < min_neighbor_height)
                min_neighbor_height = left_height;
        } else {
            /* Left edge - treat as maximum height */
            min_neighbor_height = g->height;
        }

        /* Check right neighbor */
        if (x < g->width - 1) {
            int right_height = g->relief[x + 1] + 1;
            if (right_height < min_neighbor_height)
                min_neighbor_height = right_height;
        } else {
            /* Right edge - treat as maximum height */
            min_neighbor_height = g->height;
        }

        /* Check if well depth is sufficient for Tetris */
        int well_depth = min_neighbor_height - well_height;
        if (well_depth < MIN_WELL_DEPTH)
            continue;

        /* Verify the well is relatively clear (check for obstructions) */
        bool well_clear = true;
        int clear_height = well_height + MIN_WELL_DEPTH;
        if (clear_height > g->height)
            clear_height = g->height;

        for (int y = well_height; y < clear_height; y++) {
            if (g->rows[y][x]) {
                well_clear = false;
                break;
            }
        }

        if (well_clear) {
            *well_col = x;
            return true;
        }
    }

    return false;
}
