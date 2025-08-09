/*
 * Genetic Algorithm Training Program for Tetris AI Weights
 *
 * Evolves feature weights through competitive survival tournaments.
 * Uses existing benchmark infrastructure for fitness evaluation.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "tetris.h"
#include "utils.h"

/* Genetic Algorithm Parameters */
#define POPULATION_SIZE 8      /* Number of AI candidates per generation */
#define ELITE_COUNT 2          /* Top performers kept each generation */
#define MUTATION_RATE 0.3f     /* Probability of mutating each feature */
#define MUTATION_STRENGTH 0.5f /* Maximum change per mutation */
#define CROSSOVER_RATE 0.7f    /* Probability of crossover vs pure mutation */
#define EVALUATION_GAMES 3     /* Games per fitness evaluation */
#define CLEAR_RATE_WEIGHT 0.1f /* Weight for clear rate efficiency */
#define MAX_GENERATIONS 100    /* Training generations (use -1 for infinite) */
/* Max pieces per fitness test game (need longer for LCPP) */
#define FITNESS_GAMES_LIMIT 1000

/* Feature names for logging (using existing FEAT_LIST macro) */
static const char *FEATURE_NAMES[] = {
#define _(feat) #feat,
    FEAT_LIST
#undef _
};

/* Individual AI candidate */
typedef struct {
    float weights[N_FEATIDX];
    float fitness;
    int generation;
    int games_won;
    float avg_lcpp;
    int avg_lines;
    float clear_rate;
} ai_individual_t;

/* Training statistics */
typedef struct {
    int generation;
    float best_fitness;
    float avg_fitness;
    ai_individual_t best_individual;
    int evaluations_done;
} training_stats_t;

/* Silent single game evaluation for training (no progress output) */
static game_stats_t train_evaluate_single_game(const float *weights)
{
    game_stats_t stats = {0}; /* Initialize all fields to 0 */
    uint64_t start_ns = get_time_ns();

    grid_t *g = grid_new(GRID_HEIGHT, GRID_WIDTH);
    block_t *b = block_new();
    shape_stream_t *ss = shape_stream_new();

    if (!g || !b || !ss) {
        nfree(g);
        nfree(b);
        nfree(ss);
        return stats;
    }

    int total_points = 0;
    int total_lines_cleared = 0;
    int pieces_placed = 0;
    const int MAX_PIECES = FITNESS_GAMES_LIMIT;
    const int MAX_MOVE_ATTEMPTS = 50;

    /* Initialize first block */
    shape_stream_pop(ss);
    shape_t *first_shape = shape_stream_peek(ss, 0);
    if (!first_shape)
        goto cleanup;

    block_init(b, first_shape);
    grid_block_spawn(g, b);

    if (grid_block_collides(g, b))
        goto cleanup;

    pieces_placed = 1;

    /* Main game loop - silent evaluation */
    while (pieces_placed < MAX_PIECES) {
        /* Early termination only for extremely poor performers
         * Allow more pieces for better measurement accuracy
         */
        if (pieces_placed > 800 && total_lines_cleared == 0)
            break;

        move_t *best = move_find_best(g, b, ss, weights);
        if (!best)
            break;

        /* Apply AI move */
        block_t test_block = *b;
        test_block.rot = best->rot;
        test_block.offset.x = best->col;

        if (grid_block_collides(g, &test_block))
            break;

        /* Rotation */
        int rotation_attempts = 0;
        while (b->rot != best->rot && rotation_attempts < MAX_MOVE_ATTEMPTS) {
            int old_rot = b->rot;
            grid_block_rotate(g, b, 1);
            if (b->rot == old_rot)
                break;
            rotation_attempts++;
        }

        /* Movement */
        int movement_attempts = 0;
        while (b->offset.x != best->col &&
               movement_attempts < MAX_MOVE_ATTEMPTS) {
            int old_x = b->offset.x;
            int target_col = best->col;

            /* Move multiple steps at once if far away */
            int distance = abs(target_col - b->offset.x);
            int steps = (distance > 5) ? distance / 2 : 1;

            if (b->offset.x < target_col) {
                grid_block_move(g, b, RIGHT, steps);
            } else {
                grid_block_move(g, b, LEFT, steps);
            }
            if (b->offset.x == old_x)
                break;
            movement_attempts++;
        }

        /* Drop and place */
        grid_block_drop(g, b);
        if (grid_block_collides(g, b))
            break;

        grid_block_add(g, b);

        /* Clear lines */
        int cleared = grid_clear_lines(g);
        if (cleared > 0) {
            total_lines_cleared += cleared;

            /* Track line distribution */
            if (cleared >= 1 && cleared <= 4) {
                stats.line_distribution[cleared]++;
                stats.total_clears++;
            }

            int line_points = cleared * 100;
            if (cleared > 1)
                line_points *= cleared;
            total_points += line_points;
        }

        /* Track maximum height */
        int current_max = 0;
        for (int col = 0; col < GRID_WIDTH; col++) {
            int height = g->relief[col] + 1;
            if (height > current_max)
                current_max = height;
        }
        if (current_max > stats.max_height_reached)
            stats.max_height_reached = current_max;

        /* Next piece */
        shape_stream_pop(ss);
        shape_t *next_shape = shape_stream_peek(ss, 0);
        if (!next_shape)
            break;

        block_init(b, next_shape);
        grid_block_spawn(g, b);

        if (grid_block_collides(g, b))
            break;

        pieces_placed++;
    }

    if (pieces_placed >= MAX_PIECES)
        stats.hit_piece_limit = true;

cleanup:;
    uint64_t duration_ns = get_time_ns() - start_ns;
    double duration = (double) duration_ns / 1e9;

    stats.lines_cleared = total_lines_cleared;
    stats.score = total_points;
    stats.pieces_placed = pieces_placed;
    stats.lcpp =
        pieces_placed > 0 ? (float) total_lines_cleared / pieces_placed : 0.0f;
    stats.game_duration = duration;
    stats.pieces_per_second = duration > 0 ? pieces_placed / duration : 0.0f;
    nfree(ss);
    nfree(g);
    nfree(b);

    return stats;
}

/* Efficient batch fitness evaluation for entire population */
static void evaluate_population_fitness(ai_individual_t *population,
                                        int pop_size,
                                        int eval_games)
{
    printf("  Evaluating population:\n");

    /* Pre-allocate statistics arrays for batch processing */
    game_stats_t *batch_stats = malloc(eval_games * sizeof(game_stats_t));
    if (!batch_stats) {
        fprintf(stderr, "Failed to allocate batch statistics\n");
        return;
    }

    /* Initial progress bar display */
    printf("    [                    ]   0%%");
    fflush(stdout);

    for (int i = 0; i < pop_size; i++) {
        /* Batch evaluate multiple games for this individual */
        int total_lines = 0;
        int total_pieces = 0;
        float total_lcpp = 0.0f;
        int games_completed = 0;

        int total_max_heights = 0;
        int total_clears_events = 0;

        for (int game = 0; game < eval_games; game++) {
            batch_stats[game] =
                train_evaluate_single_game(population[i].weights);

            total_lines += batch_stats[game].lines_cleared;
            total_pieces += batch_stats[game].pieces_placed;
            total_lcpp += batch_stats[game].lcpp;
            total_max_heights += batch_stats[game].max_height_reached;
            total_clears_events += batch_stats[game].total_clears;

            if (batch_stats[game].pieces_placed >= FITNESS_GAMES_LIMIT * 0.8f)
                games_completed++;
        }

        /* Calculate composite fitness score with optimized formula */
        population[i].avg_lines = total_lines / eval_games;
        population[i].avg_lcpp = total_lcpp / eval_games;
        population[i].games_won = games_completed;

        /* Clear rate: clears per piece as efficiency proxy */
        population[i].clear_rate =
            (total_pieces > 0) ? (float) total_clears_events / total_pieces
                               : 0.0f;

        /* Enhanced fitness formula prioritizing efficiency */
        float survival_ratio =
            (float) total_pieces / (FITNESS_GAMES_LIMIT * eval_games);
        float completion_ratio = (float) games_completed / eval_games;

        /* Weighted fitness components */
        float lcpp_score = population[i].avg_lcpp * 2000.0f;
        float efficiency_bonus =
            (population[i].avg_lcpp > 0.25f) ? 200.0f : 0.0f;
        float line_score = (float) total_lines * 0.5f;

        /* Enhanced survival bonus considering max height reached */
        float avg_max_height = (float) total_max_heights / eval_games;
        float height_factor = (avg_max_height > 5.0f)
                                  ? fmaxf(0.2f, 10.0f / avg_max_height)
                                  : 2.0f;
        float survival_bonus = survival_ratio * 50.0f * height_factor;

        float completion_bonus = completion_ratio * 25.0f;

        /* Clear efficiency bonus (proxy for good search efficiency) */
        float clear_efficiency_bonus = population[i].clear_rate * 100.0f;

        /* Height penalty - penalize high stacks more severely */
        float height_penalty =
            (avg_max_height > 15.0f) ? -20.0f * (avg_max_height - 15.0f) : 0.0f;

        float efficiency_penalty =
            (population[i].avg_lcpp < 0.15f) ? -300.0f : 0.0f;

        population[i].fitness = lcpp_score + efficiency_bonus + line_score +
                                survival_bonus + completion_bonus +
                                clear_efficiency_bonus + height_penalty +
                                efficiency_penalty;

        /* Update progress bar after evaluation with colors */
        int progress_chars = ((i + 1) * 20) / pop_size;
        int percent = ((i + 1) * 100) / pop_size;
        printf("\r    [");

        /* Green filled portion */
        printf("\x1b[32m"); /* Green color */
        for (int j = 0; j < progress_chars; j++)
            printf("█");
        printf("\x1b[0m"); /* Reset color */

        /* Empty portion */
        for (int j = progress_chars; j < 20; j++)
            printf(" ");
        printf("] %3d%%", percent);
        fflush(stdout);
    }

    /* Complete the progress bar and move to next line */
    printf(" - Complete!\n");
    free(batch_stats);
}

/* Initialize individual with optimized weight selection */
static void init_individual(ai_individual_t *individual, int generation)
{
    /* Use existing default weights as baseline */
    float *defaults = move_defaults();
    if (!defaults) {
        /* Fallback if defaults unavailable */
        for (int i = 0; i < N_FEATIDX; i++)
            individual->weights[i] = ((float) rand() / RAND_MAX - 0.5f) * 2.0f;
    } else {
        for (int i = 0; i < N_FEATIDX; i++) {
            /* Add controlled random variation to proven weights */
            float variation = ((float) rand() / RAND_MAX - 0.5f) * 0.05f;
            individual->weights[i] = defaults[i] + variation;
        }
        free(defaults);
    }

    individual->fitness = 0.0f;
    individual->generation = generation;
    individual->games_won = 0;
    individual->avg_lcpp = 0.0f;
    individual->avg_lines = 0;
}

/* Adaptive mutation with feature-specific constraints */
static void mutate_individual(ai_individual_t *individual,
                              int generation,
                              float mut_rate)
{
    /* Feature-specific constraints based on Tetris AI analysis */
    const float min_bounds[] = {
        [FEATIDX_RELIEF_MAX] = -2.0f, [FEATIDX_RELIEF_AVG] = -5.0f,
        [FEATIDX_RELIEF_VAR] = -2.0f, [FEATIDX_GAPS] = -4.0f,
        [FEATIDX_OBS] = -3.0f,        [FEATIDX_DISCONT] = -2.0f,
        [FEATIDX_CREVICES] = -4.0f, /* Crevices strongly penalized */
    };
    const float max_bounds[] = {
        [FEATIDX_RELIEF_MAX] = 1.0f, [FEATIDX_RELIEF_AVG] = -0.5f,
        [FEATIDX_RELIEF_VAR] = 1.0f, [FEATIDX_GAPS] = 0.0f,
        [FEATIDX_OBS] = 0.0f,        [FEATIDX_DISCONT] = 1.0f,
        [FEATIDX_CREVICES] = -0.5f, /* Always negative penalty */
    };

    for (int i = 0; i < N_FEATIDX; i++) {
        if ((float) rand() / RAND_MAX < mut_rate) {
            float change =
                ((float) rand() / RAND_MAX - 0.5f) * 2.0f * MUTATION_STRENGTH;
            individual->weights[i] += change;

            /* Apply bounds */
            if (individual->weights[i] > max_bounds[i])
                individual->weights[i] = max_bounds[i];
            if (individual->weights[i] < min_bounds[i])
                individual->weights[i] = min_bounds[i];
        }
    }
    individual->generation = generation;
}

/* Optimized crossover with blend ratio variation */
static void crossover_individuals(const ai_individual_t *parent1,
                                  const ai_individual_t *parent2,
                                  ai_individual_t *child,
                                  int generation)
{
    /* Use fitness-weighted blending for better offspring */
    float total_fitness = parent1->fitness + parent2->fitness;
    float alpha = (total_fitness > 0) ? parent1->fitness / total_fitness : 0.5f;

    /* Add some randomness to prevent premature convergence */
    alpha += ((float) rand() / RAND_MAX - 0.5f) * 0.2f;
    alpha = fmaxf(0.1f, fminf(0.9f, alpha)); /* Clamp to reasonable range */

    for (int i = 0; i < N_FEATIDX; i++) {
        child->weights[i] =
            alpha * parent1->weights[i] + (1.0f - alpha) * parent2->weights[i];
    }
    child->generation = generation;
}

/* Tournament selection with size adaptation */
static int tournament_select(const ai_individual_t *population,
                             int pop_size,
                             int tournament_size)
{
    int best_idx = rand_range(pop_size);
    float best_fitness = population[best_idx].fitness;

    for (int i = 1; i < tournament_size; i++) {
        int candidate = rand_range(pop_size);
        if (population[candidate].fitness > best_fitness) {
            best_idx = candidate;
            best_fitness = population[candidate].fitness;
        }
    }
    return best_idx;
}

/* Efficient sorting comparison */
static int compare_fitness(const void *a, const void *b)
{
    const ai_individual_t *ia = (const ai_individual_t *) a;
    const ai_individual_t *ib = (const ai_individual_t *) b;

    /* Use epsilon for floating point comparison */
    float diff = ia->fitness - ib->fitness;
    if (diff > 0.001f)
        return -1;
    if (diff < -0.001f)
        return 1;
    return 0;
}

/* Optimized individual printing */
static void print_individual(const ai_individual_t *individual,
                             const char *label)
{
    printf("%s (Gen %d, Fitness: %.2f, LCPP: %.3f, Lines: %d, Won: %d):\n",
           label, individual->generation, individual->fitness,
           individual->avg_lcpp, individual->avg_lines, individual->games_won);

    printf("  Weights: [");
    for (int i = 0; i < N_FEATIDX; i++) {
        printf("%.3f%s", individual->weights[i],
               (i < N_FEATIDX - 1) ? ", " : "]\n");
    }
}

/* Generate optimized C code output */
static void print_c_weights(const ai_individual_t *best)
{
    printf("\n/* Evolved weights (Generation %d, Fitness: %.2f) */\n",
           best->generation, best->fitness);
    printf("static const float evolved_weights[N_FEATIDX] = {\n");
    for (int i = 0; i < N_FEATIDX; i++) {
        printf("    [FEATIDX_%s] = %.4ff,\n", FEATURE_NAMES[i],
               best->weights[i]);
    }
    printf("};\n");
}

/* Main training loop with performance optimizations */
static void train_weights(int max_generations,
                          int pop_size,
                          int eval_games,
                          float mut_rate)
{
    /* Use nalloc for better memory management */
    ai_individual_t *population =
        nalloc(pop_size * sizeof(ai_individual_t), NULL);
    ai_individual_t *next_population =
        nalloc(pop_size * sizeof(ai_individual_t), NULL);

    if (!population || !next_population) {
        fprintf(stderr, "Failed to allocate population memory\n");
        nfree(population);
        nfree(next_population);
        return;
    }

    training_stats_t stats = {0};
    int elite_count = (pop_size >= 4) ? 2 : 1;

    printf("Tetris AI Weight Evolution Training\n");
    printf("===================================\n");
    printf("Population: %d, Evaluation Games: %d, Max Generations: %d\n",
           pop_size, eval_games, max_generations);
    printf("Elite Count: %d, Mutation Rate: %.2f\n\n", elite_count, mut_rate);

    /* Initialize population */
    for (int i = 0; i < pop_size; i++)
        init_individual(&population[i], 0);

    /* Evolution loop */
    for (int generation = 0;
         generation < max_generations || max_generations < 0; generation++) {
        printf("Generation %d:\n", generation);
        printf("-------------\n");

        /* Efficient batch evaluation */
        evaluate_population_fitness(population, pop_size, eval_games);
        stats.evaluations_done += pop_size;

        /* Sort by fitness */
        qsort(population, pop_size, sizeof(ai_individual_t), compare_fitness);

        /* Update statistics */
        stats.generation = generation;
        stats.best_fitness = population[0].fitness;

        float total_fitness = 0.0f;
        for (int i = 0; i < pop_size; i++)
            total_fitness += population[i].fitness;
        stats.avg_fitness = total_fitness / pop_size;
        stats.best_individual = population[0];

        /* Report results */
        print_individual(&population[0], "Best");
        printf("  Average Fitness: %.2f\n", stats.avg_fitness);
        printf("  Evaluations Done: %d\n\n", stats.evaluations_done);

        /* Early termination for excellent solutions */
        if (population[0].fitness > 600.0f && population[0].avg_lcpp > 0.32f) {
            printf("Excellent solution found! Stopping early.\n\n");
            break;
        }

        /* Create next generation with elitism */
        for (int i = 0; i < elite_count; i++)
            next_population[i] = population[i];

        /* Generate offspring */
        for (int i = elite_count; i < pop_size; i++) {
            if ((float) rand() / RAND_MAX < CROSSOVER_RATE) {
                /* Crossover with adaptive tournament size */
                int tournament_size = (generation < 10) ? 2 : 3;
                int parent1 =
                    tournament_select(population, pop_size, tournament_size);
                int parent2 =
                    tournament_select(population, pop_size, tournament_size);
                crossover_individuals(&population[parent1],
                                      &population[parent2], &next_population[i],
                                      generation + 1);
            } else {
                /* Pure mutation */
                int parent = tournament_select(population, pop_size, 2);
                next_population[i] = population[parent];
            }

            /* Apply mutation */
            mutate_individual(&next_population[i], generation + 1, mut_rate);
        }

        /* Replace population */
        memcpy(population, next_population, pop_size * sizeof(ai_individual_t));

        /* Periodic weight saving */
        if (generation % 10 == 9) {
            char filename[64];
            snprintf(filename, sizeof(filename), "weights_gen_%d.txt",
                     generation + 1);
            FILE *f = fopen(filename, "w");
            if (f) {
                for (int i = 0; i < N_FEATIDX; i++)
                    fprintf(f, "%.6f\n", population[0].weights[i]);
                fclose(f);
                printf("Saved weights to %s\n\n", filename);
            }
        }
    }

    /* Final results */
    printf("\nTraining Complete!\n");
    printf("==================\n");
    print_individual(&stats.best_individual, "Final Best Individual");
    print_c_weights(&stats.best_individual);

    /* Save final weights */
    FILE *f = fopen("evolved_weights.h", "w");
    if (f) {
        fprintf(f, "/* Evolved weights (Generation %d, Fitness: %.2f) */\n",
                stats.best_individual.generation,
                stats.best_individual.fitness);
        fprintf(f, "#pragma once\n\n");
        fprintf(f, "#include \"tetris.h\"\n\n");
        fprintf(f, "static const float evolved_weights[N_FEATIDX] = {\n");
        for (int i = 0; i < N_FEATIDX; i++) {
            fprintf(f, "    [FEATIDX_%s] = %.4ff,\n", FEATURE_NAMES[i],
                    stats.best_individual.weights[i]);
        }
        fprintf(f, "};\n");
        fclose(f);
        printf("\nEvolved weights saved to evolved_weights.h\n");
    }

    nfree(population);
    nfree(next_population);
}

/* Usage information */
static void print_usage(const char *program_name)
{
    printf("Usage: %s [options]\n", program_name);
    printf("Options:\n");
    printf("  -g N      Maximum generations (default: %d, -1 for infinite)\n",
           MAX_GENERATIONS);
    printf("  -p N      Population size (default: %d)\n", POPULATION_SIZE);
    printf("  -e N      Evaluation games per individual (default: %d)\n",
           EVALUATION_GAMES);
    printf("  -m RATE   Mutation rate 0.0-1.0 (default: %.2f)\n",
           MUTATION_RATE);
    printf("  -s SEED   Random seed (default: time-based)\n");
    printf("  -h        Show this help\n");
    printf("\nExample:\n");
    printf(
        "  %s -g 50 -p 12 -e 5    # 50 generations, 12 individuals, 5 games "
        "each\n",
        program_name);
}

/* Main program */
int main(int argc, char *argv[])
{
    int max_generations = MAX_GENERATIONS;
    int population_size = POPULATION_SIZE;
    int evaluation_games = EVALUATION_GAMES;
    float mutation_rate = MUTATION_RATE;
    unsigned int seed = (unsigned int) time(NULL);

    /* Parse command line options */
    int opt;
    while ((opt = getopt(argc, argv, "g:p:e:m:s:h")) != -1) {
        switch (opt) {
        case 'g':
            max_generations = atoi(optarg);
            break;
        case 'p':
            population_size = atoi(optarg);
            if (population_size < 2 || population_size > 50) {
                fprintf(stderr, "Population size must be between 2 and 50\n");
                return 1;
            }
            break;
        case 'e':
            evaluation_games = atoi(optarg);
            if (evaluation_games < 1 || evaluation_games > 20) {
                fprintf(stderr, "Evaluation games must be between 1 and 20\n");
                return 1;
            }
            break;
        case 'm':
            mutation_rate = atof(optarg);
            if (mutation_rate < 0.0f || mutation_rate > 1.0f) {
                fprintf(stderr, "Mutation rate must be between 0.0 and 1.0\n");
                return 1;
            }
            break;
        case 's':
            seed = (unsigned int) atoi(optarg);
            break;
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:
            fprintf(stderr, "Unknown option. Use -h for help.\n");
            return 1;
        }
    }

    /* Initialize systems */
    grid_init();
    if (!shape_init()) {
        fprintf(stderr, "Failed to initialize shapes\n");
        return 1;
    }

    srand(seed);
    atexit(shape_free);

    /* Start optimized training */
    train_weights(max_generations, population_size, evaluation_games,
                  mutation_rate);

    return 0;
}
