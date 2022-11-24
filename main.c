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
        fprintf(stderr, "Failed to open %s", shapes_file);
        return 1;
    }

    srand(time(NULL) ^ getpid());

    float *w = default_weights();
    auto_play(w);
    free(w);

    return 0;
}
