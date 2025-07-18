#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

/* ANSI color macros for test output */
#define COLOR_RESET "\033[0m"
#define COLOR_RED "\033[91m"
#define COLOR_GREEN "\033[92m"
#define COLOR_YELLOW "\033[93m"
#define COLOR_CYAN "\033[96m"
#define COLOR_WHITE "\033[42m"
#define COLOR_BOLD "\033[1m"

/* Test output wrapper macros */
#define TEST_FAIL_PREFIX COLOR_RED "FAIL:" COLOR_RESET " "
#define TEST_PASS_PREFIX COLOR_GREEN "PASS:" COLOR_RESET " "
#define TEST_SUMMARY_FORMAT                                  \
    COLOR_YELLOW "%zu TESTS" COLOR_RESET "  /  " COLOR_GREEN \
                 "%zu PASSED" COLOR_RESET "  /  " COLOR_RED  \
                 "%zu FAILED" COLOR_RESET "\n"
#define TEST_ALL_PASSED COLOR_WHITE "ALL %zu TESTS PASSED" COLOR_RESET "\n"
#define TEST_CATEGORY_HEADER COLOR_BOLD COLOR_CYAN "=== %s ===" COLOR_RESET "\n"
#define TEST_CATEGORY_FOOTER COLOR_CYAN "%s completed" COLOR_RESET "\n\n"

/* Test function declarations */
bool assert_test(bool condition, const char *format, ...);
void start_test_category(const char *name);
void end_test_category(const char *name);
