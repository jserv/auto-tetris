#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "tetris.h"

#define DATADIR "data"

int main()
{
    char *shapes_file = DATADIR "/shapes";
    if (!shapes_init(shapes_file)) {
        fprintf(stderr, "Failed to open %s\n", shapes_file);
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

    auto_play(w);
    free(w);

    return 0;
}
