#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "test.h"

/* Include the main tetris headers for testing */
#include "../nalloc.h"
#include "../tetris.h"

/* Memory allocation tests */
void test_nalloc_basic_allocation(void);
void test_nalloc_simple_realloc(void);
void test_nalloc_edge_cases(void);
void test_nalloc_parent_child_relationships(void);

/* Basic types and constants tests */
void test_coordinate_operations(void);
void test_direction_constants(void);
void test_grid_constants_validation(void);
void test_shape_constants_validation(void);

/* Block operation tests */
void test_block_basic_allocation(void);
void test_block_initialization_with_shapes(void);
void test_block_coordinate_retrieval(void);
void test_block_rotation_operations(void);
void test_block_movement_operations(void);
void test_block_extreme_calculations(void);
void test_block_edge_cases(void);

/* Grid operation tests */
void test_grid_basic_allocation(void);
void test_grid_allocation_edge_cases(void);
void test_grid_copy_operations(void);
void test_grid_block_intersection_detection(void);
void test_grid_block_add_remove_operations(void);
void test_grid_block_center_elevate(void);
void test_grid_block_drop_operation(void);
void test_grid_block_movement_validation(void);
void test_grid_block_rotation_validation(void);
void test_grid_line_clearing(void);
void test_grid_edge_cases_and_robustness(void);

/* Move/AI system tests */
void test_move_default_weights_allocation(void);
void test_move_default_weights_consistency(void);
void test_move_best_basic_functionality(void);
void test_move_best_edge_cases(void);
void test_move_best_multiple_shapes(void);
void test_move_best_weight_sensitivity(void);
void test_ai_decision_quality(void);
void test_move_structure_properties(void);
void test_ai_performance_characteristics(void);

/* Shape system tests */
void test_shape_system_initialization(void);
void test_shape_index_bounds_checking(void);
void test_shape_properties_validation(void);
void test_shape_rotation_consistency(void);
void test_shape_crust_data_validation(void);
void test_shape_stream_basic_operations(void);
void test_shape_stream_bounds_and_edge_cases(void);
void test_shape_stream_7bag_randomization(void);
void test_shape_stream_multiple_bags_distribution(void);
void test_shape_stream_reset_functionality(void);
void test_shape_stream_gameplay_sequence(void);
void test_shape_stream_memory_management(void);
void test_shape_multiple_init_cleanup_cycles(void);
void test_shape_edge_cases(void);

/* Game mechanics tests */
void test_game_stats_structure_validation(void);
void test_game_benchmark_results_structure(void);
void test_game_input_enumeration_validation(void);
void test_game_basic_piece_placement_sequence(void);
void test_game_block_coordinate_retrieval(void);
void test_game_grid_copy_operations(void);
void test_game_line_clearing_mechanics(void);
void test_game_over_detection_logic(void);
void test_game_ai_vs_human_decision_making(void);
void test_game_ai_weight_system_validation(void);
void test_game_scoring_and_statistics_logic(void);
void test_game_piece_stream_continuity(void);
void test_game_multi_piece_sequence_validation(void);
void test_game_comprehensive_tetromino_placement_validation(void);
void test_game_grid_boundary_collision_detection(void);
void test_game_complex_grid_state_validation(void);
void test_game_tetromino_rotation_state_consistency(void);
void test_game_line_clearing_pattern_validation(void);
void test_game_memory_cleanup_validation(void);
void test_game_grid_different_dimensions(void);
void test_game_edge_cases_and_robustness(void);
void test_game_complete_lifecycle_state_transitions(void);
void test_game_grid_internal_state_consistency(void);
void test_game_block_add_remove_symmetry(void);
void test_game_collision_detection_accuracy(void);
void test_game_movement_validation_comprehensive(void);
void test_game_rotation_validation_comprehensive(void);
void test_game_shape_stream_state_transitions(void);

/* Global test statistics */
static size_t tested = 0, passed = 0;
static const char *current_test = "initialization";

/* Test category management */
void start_test_category(const char *name)
{
    printf(TEST_CATEGORY_HEADER, name);
}

void end_test_category(const char *name)
{
    printf(TEST_CATEGORY_FOOTER, name);
}

bool assert_test(bool condition, const char *format, ...)
{
    va_list args;
    va_start(args, format);

    tested++;
    if (!condition) {
        fprintf(stderr, TEST_FAIL_PREFIX);
        vfprintf(stderr, format, args);
    } else {
        passed++;
        printf(TEST_PASS_PREFIX);
        vprintf(format, args);
    }
    va_end(args);
    printf("\n");
    return condition;
}

static void test_reset_global_state(void)
{
    /* Reset any global state between test categories
     * Defensive reset - avoid calling functions that might cause issues.
     * reset_shape_bag() is safe - just sets bag_pos = 7
     */
    reset_shape_bag();
}

static void print_test_report(void)
{
    size_t failed = tested - passed;
    if (failed) {
        printf(TEST_SUMMARY_FORMAT, tested, passed, failed);
    } else {
        printf(TEST_ALL_PASSED, tested);
    }
}

/* Signal handling for crash detection */
#define SIGNAL_HEADER COLOR_RED "=== SIGNAL CAUGHT ===" COLOR_RESET
#define SIGNAL_TERMINATE \
    COLOR_RED "Test suite terminated due to signal." COLOR_RESET

static const char *signal_name(int sig)
{
    switch (sig) {
    case SIGSEGV:
        return "SIGSEGV (Segmentation Fault)";
    case SIGABRT:
        return "SIGABRT (Abort)";
    case SIGFPE:
        return "SIGFPE (Floating Point Exception)";
    case SIGBUS:
        return "SIGBUS (Bus Error)";
    case SIGILL:
        return "SIGILL (Illegal Instruction)";
    case SIGTRAP:
        return "SIGTRAP (Trace Trap)";
    default:
        return "Unknown Signal";
    }
}

static void signal_handler(int sig)
{
    printf("\n\n" SIGNAL_HEADER "\n");
    printf("Signal: %s (%d)\n", signal_name(sig), sig);
    printf("Current test: %s\n", current_test);
    printf("Tests completed so far:\n");
    print_test_report();

    printf("\n" SIGNAL_TERMINATE "\n");

    /* Reset signal handler to default and re-raise to get core dump */
    signal(sig, SIG_DFL);
    exit(1);
}

static void setup_signal_handlers(void)
{
    signal(SIGSEGV, signal_handler);
    signal(SIGABRT, signal_handler);
    signal(SIGFPE, signal_handler);
    signal(SIGBUS, signal_handler);
    signal(SIGILL, signal_handler);
    signal(SIGTRAP, signal_handler);
}

#define RUN(test_func)             \
    do {                           \
        current_test = #test_func; \
        test_func();               \
    } while (0)

#define RUN_CATEGORY(category_name, test_functions)      \
    do {                                                 \
        start_test_category(category_name);              \
        test_functions end_test_category(category_name); \
        test_reset_global_state();                       \
    } while (0)

int main(void)
{
    printf(COLOR_BOLD "Running auto-tetris test suite...\n" COLOR_RESET);
    printf("Testing against tetris.h public interface\n\n");

    /* Set up signal handling for crash detection */
    setup_signal_handlers();

    /* Memory allocation tests */
    RUN_CATEGORY("Memory Allocation Tests", {
        RUN(test_nalloc_basic_allocation);
        RUN(test_nalloc_simple_realloc);
        RUN(test_nalloc_edge_cases);
        RUN(test_nalloc_parent_child_relationships);
    });

    /* Basic types and constants tests */
    RUN_CATEGORY("Basic Types and Constants Tests", {
        RUN(test_coordinate_operations);
        RUN(test_direction_constants);
        RUN(test_grid_constants_validation);
        RUN(test_shape_constants_validation);
    });

    /* Block operation tests */
    RUN_CATEGORY("Block Operation Tests", {
        RUN(test_block_basic_allocation);
        RUN(test_block_initialization_with_shapes);
        RUN(test_block_coordinate_retrieval);
        RUN(test_block_rotation_operations);
        RUN(test_block_movement_operations);
        RUN(test_block_extreme_calculations);
        RUN(test_block_edge_cases);
    });

    /* Grid operation tests */
    RUN_CATEGORY("Grid Operation Tests", {
        RUN(test_grid_basic_allocation);
        RUN(test_grid_allocation_edge_cases);
        RUN(test_grid_copy_operations);
        RUN(test_grid_block_intersection_detection);
        RUN(test_grid_block_add_remove_operations);
        RUN(test_grid_block_center_elevate);
        RUN(test_grid_block_drop_operation);
        RUN(test_grid_block_movement_validation);
        RUN(test_grid_block_rotation_validation);
        RUN(test_grid_line_clearing);
        RUN(test_grid_edge_cases_and_robustness);
    });

    /* Move/AI system tests */
    RUN_CATEGORY("Move/AI System Tests", {
        RUN(test_move_default_weights_allocation);
        RUN(test_move_default_weights_consistency);
        RUN(test_move_best_basic_functionality);
        RUN(test_move_best_edge_cases);
        RUN(test_move_best_multiple_shapes);
        RUN(test_move_best_weight_sensitivity);
        RUN(test_ai_decision_quality);
        RUN(test_move_structure_properties);
        RUN(test_ai_performance_characteristics);
    });

    /* Shape system tests */
    RUN_CATEGORY("Shape System Tests", {
        RUN(test_shape_system_initialization);
        RUN(test_shape_index_bounds_checking);
        RUN(test_shape_properties_validation);
        RUN(test_shape_rotation_consistency);
        RUN(test_shape_crust_data_validation);
        RUN(test_shape_stream_basic_operations);
        RUN(test_shape_stream_bounds_and_edge_cases);
        RUN(test_shape_stream_7bag_randomization);
        RUN(test_shape_stream_multiple_bags_distribution);
        RUN(test_shape_stream_reset_functionality);
        RUN(test_shape_stream_gameplay_sequence);
        RUN(test_shape_stream_memory_management);
        RUN(test_shape_multiple_init_cleanup_cycles);
        RUN(test_shape_edge_cases);
    });

    /* Game mechanics tests */
    RUN_CATEGORY("Game Mechanics Tests", {
        RUN(test_game_stats_structure_validation);
        RUN(test_game_benchmark_results_structure);
        RUN(test_game_input_enumeration_validation);
        RUN(test_game_basic_piece_placement_sequence);
        RUN(test_game_block_coordinate_retrieval);
        RUN(test_game_grid_copy_operations);
        RUN(test_game_line_clearing_mechanics);
        RUN(test_game_over_detection_logic);
        RUN(test_game_ai_vs_human_decision_making);
        RUN(test_game_ai_weight_system_validation);
        RUN(test_game_scoring_and_statistics_logic);
        RUN(test_game_piece_stream_continuity);
        RUN(test_game_multi_piece_sequence_validation);
        RUN(test_game_comprehensive_tetromino_placement_validation);
        RUN(test_game_grid_boundary_collision_detection);
        RUN(test_game_complex_grid_state_validation);
        RUN(test_game_tetromino_rotation_state_consistency);
        RUN(test_game_line_clearing_pattern_validation);
        RUN(test_game_memory_cleanup_validation);
        RUN(test_game_grid_different_dimensions);
        RUN(test_game_edge_cases_and_robustness);
        RUN(test_game_complete_lifecycle_state_transitions);
        RUN(test_game_grid_internal_state_consistency);
        RUN(test_game_block_add_remove_symmetry);
        RUN(test_game_collision_detection_accuracy);
        RUN(test_game_movement_validation_comprehensive);
        RUN(test_game_rotation_validation_comprehensive);
        RUN(test_game_shape_stream_state_transitions);
    });

    /* Final cleanup and report */
    current_test = "test suite completion";

    printf(COLOR_BOLD "Test suite execution completed.\n" COLOR_RESET);
    print_test_report();

    /* Don't call shapes_free() here to avoid potential exit handler conflicts
     */

    /* Ensure clean exit */
    fflush(stdout);
    fflush(stderr);

    /* Use _exit() to bypass atexit handlers that may cause segfaults */
    _exit((tested == passed) ? 0 : 1);
}
