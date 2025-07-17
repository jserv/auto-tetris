#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "tetris.h"

static void print_usage(const char *program_name)
{
    printf("Usage: %s [options]\n", program_name);
    printf("Options:\n");
    printf(
        "  -b [N]    Run benchmark mode with N games (default: 1, max: "
        "1000)\n");
    printf("  -h        Show this help message\n");
    printf("\nBenchmark mode measures AI performance with these metrics:\n");
    printf("  - Lines Cleared: Total lines cleared before game over\n");
    printf("  - Score: Final score achieved\n");
    printf("  - Pieces Placed: Number of pieces used\n");
    printf("  - LCPP: Lines Cleared Per Piece (efficiency metric)\n");
    printf("  - Game Duration: Time taken to complete the game\n");
    printf("  - Pieces per Second: Decision-making speed\n");
    printf("\nUsage examples:\n");
    printf("  %s -b        # Single test (1 game)\n", program_name);
    printf("  %s -b 10     # Comprehensive test (10 games)\n", program_name);
    printf("\nEvaluation features:\n");
    printf("  - Performance rating against known AI benchmarks\n");
    printf("  - Consistency analysis (natural vs artificial game endings)\n");
    printf("  - Speed analysis for real-time gameplay suitability\n");
    printf("  - Statistical analysis with standard deviation\n");
    printf("  - Personalized recommendations for improvement\n");
}

int main(int argc, char *argv[])
{
    bool bench_mode = false;
    int bench_games = 1; /* Default number of benchmark games */

    /* Parse command line arguments */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-b")) {
            bench_mode = true;

            /* Check if next argument is a number */
            if (i + 1 < argc) {
                char *endptr;
                long games = strtol(argv[i + 1], &endptr, 10);

                /* If it's a valid number, use it */
                if (*endptr == '\0' && games > 0 && games <= 1000) {
                    bench_games = (int) games;
                    i++; /* Skip the number argument */
                } else if (*endptr != '\0') {
                    /* Not a number, treat as next option */
                    /* Don't increment i, let next iteration handle it */
                }
            }
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    /* Initialize shapes */
    if (!shapes_init()) {
        fprintf(stderr, "Failed to initialize shapes\n");
        return 1;
    }

    /* Register cleanup functions for proper memory management */
    move_cleanup_atexit();

    /* Initialize random seed */
    srand(time(NULL) ^ getpid());

    /* Get default AI weights */
    float *w = default_weights();
    if (!w) {
        fprintf(stderr, "Failed to allocate weights\n");
        return 1;
    }

    if (bench_mode) {
        /* Run benchmark mode */
        printf("Tetris AI Benchmark Mode\n");
        printf("========================\n");
        printf("Grid Size: %dx%d\n", GRID_WIDTH, GRID_HEIGHT);

        bench_results_t results = bench_run(w, bench_games);
        bench_print_results(&results);

        /* Cleanup benchmark results */
        free(results.games);
    } else {
        /* Run normal interactive mode */
        auto_play(w);
    }

    free(w);
    return 0;
}
