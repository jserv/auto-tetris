#include <stdio.h>
#include <stdlib.h>

#include "tetris.h"
#include "utils.h"

void block_init(block_t *b, shape_t *s)
{
    b->rot = 0;
    b->offset.x = 0, b->offset.y = 0;
    b->shape = s;
}

block_t *block_new(void)
{
    block_t *b = nalloc(sizeof(block_t), NULL);
    block_init(b, NULL);
    return b;
}

void block_get(const block_t *b, int i, coord_t *result)
{
    /* Add bounds checking to prevent memory corruption */
    if (!b || !b->shape || !result || i < 0 || i >= MAX_BLOCK_LEN) {
        if (result) {
            result->x = -1;
            result->y = -1;
        }
        return;
    }

    /* Validate rotation index */
    if (b->rot < 0 || b->rot >= 4) {
        result->x = -1;
        result->y = -1;
        return;
    }

    /* Fast path for default rotation */
    if (b->rot == 0) {
        int x = b->shape->rot_flat[0][i][0];
        int y = b->shape->rot_flat[0][i][1];

        /* Check for invalid coordinates: negative values mark invalid */
        if (x < 0 || y < 0) {
            result->x = -1;
            result->y = -1;
            return;
        }

        result->x = x + b->offset.x;
        result->y = y + b->offset.y;
        return;
    }

    /* Slower path for non-default rotations with pointer validation */
    if (!b->shape->rot[b->rot] || !b->shape->rot[b->rot][i]) {
        result->x = -1;
        result->y = -1;
        return;
    }

    int *rot = b->shape->rot[b->rot][i];
    result->x = rot[0] + b->offset.x;
    result->y = rot[1] + b->offset.y;
}

int block_extreme(const block_t *b, direction_t d)
{
    switch (d) {
    case LEFT:
        return b->offset.x;
    case BOT:
        return b->offset.y;
    case RIGHT:
        return b->shape->rot_wh[b->rot].x + b->offset.x - 1;
    case TOP:
        return b->shape->rot_wh[b->rot].y + b->offset.y - 1;
    default:
        return 0;
    }
};

void block_move(block_t *b, direction_t d, int amount)
{
    switch (d) {
    case LEFT:
        b->offset.x -= amount;
        break;
    case RIGHT:
        b->offset.x += amount;
        break;
    case BOT:
        b->offset.y -= amount;
        break;
    case TOP:
        b->offset.y += amount;
        break;
    }
}

void block_rotate(block_t *b, int amount)
{
    int rot = b->shape->n_rot;
    b->rot = (b->rot + amount) % rot;
    if (b->rot < 0)
        b->rot += rot;
}
