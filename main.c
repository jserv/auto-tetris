#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "tetris.h"

int main()
{
    if (!shapes_init()) {
        fprintf(stderr, "Failed to initialize shapes\n");
        return 1;
    }

    /* Register cleanup functions for proper memory management */
    move_cleanup_atexit();

    srand(time(NULL) ^ getpid());

    float *w = default_weights();
    if (!w) {
        fprintf(stderr, "Failed to allocate weights\n");
        return 1;
    }

    /* Start the game */
    auto_play(w);
    free(w);

    return 0;
}
