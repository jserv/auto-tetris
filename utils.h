#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>

/* High-resolution timing
 * get_time_ns() returns a monotonic timestamp in nanoseconds.
 */
#if defined(__APPLE__) && defined(__MACH__)
#include <mach/mach_time.h>
static inline uint64_t get_time_ns(void)
{
    static mach_timebase_info_data_t tb;
    if (tb.denom == 0)
        (void) mach_timebase_info(&tb);
    return mach_absolute_time() * tb.numer / tb.denom;
}
#else
#include <time.h>
static inline uint64_t get_time_ns(void)
{
    struct timespec ts;
#if defined(CLOCK_MONOTONIC_RAW)
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    return (uint64_t) ts.tv_sec * 1000000000ULL + (uint64_t) ts.tv_nsec;
}
#endif

/* Platform detection for random number generation */
#if defined(__GLIBC__) && \
    (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 36))
#include <sys/random.h> /* arc4random_uniform in glibc >=2.36 */
#endif

/* Compiler Hints and Optimization Macros */

/* Branch prediction hints for better optimization */
#if defined(__GNUC__) || defined(__clang__)
#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define LIKELY(x) (x)
#define UNLIKELY(x) (x)
#endif

/* Common utility macros */
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

/* Bit Manipulation Operations */

/* Count Trailing Zeros (CTZ)
 * Returns the number of trailing zero bits in x.
 * Undefined behavior if x == 0.
 */
#if defined(__GNUC__) || defined(__clang__)
#define CTZ(x) __builtin_ctz(x)
#else
static inline int __ctz_fallback(unsigned x)
{
    if (UNLIKELY(x == 0))
        return 32;

    int count = 0;
    while ((x & 1) == 0) {
        x >>= 1;
        count++;
    }
    return count;
}
#define CTZ(x) __ctz_fallback(x)
#endif

/* Population Count (POPCOUNT)
 * Returns the number of set bits in x.
 */
#if defined(__GNUC__) || defined(__clang__)
#define POPCOUNT(x) __builtin_popcount(x)
#elif defined(_MSC_VER)
#include <intrin.h>
#define POPCOUNT(x) __popcnt(x)
#else
static inline int __popcount_fallback(unsigned x)
{
    int count = 0;
    while (x) {
        x &= x - 1; /* Clear the lowest set bit */
        count++;
    }
    return count;
}
#define POPCOUNT(x) __popcount_fallback(x)
#endif

/* High-Quality Random Number Generation */

/* Generate bias-free uniform random integer in range [0, upper)
 *
 * Uses platform-specific high-quality generators when available:
 * - arc4random_uniform() on BSD/macOS and modern glibc
 * - Rejection sampling fallback to eliminate modulo bias
 * @upper : Upper bound (exclusive), must be > 0
 *
 * Return Random integer in [0, upper)
 */
static inline uint32_t rand_range(uint32_t upper)
{
    if (UNLIKELY(upper == 0))
        return 0;

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__) ||                                                \
    (defined(__GLIBC__) &&                                                \
     (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 36)))
    /* arc4random_uniform() is cryptographically secure and bias-free */
    return arc4random_uniform(upper);
#else
    /* Rejection sampling to eliminate modulo bias */
    uint32_t limit = RAND_MAX - (RAND_MAX % upper);
    uint32_t r;

    do {
        r = (uint32_t) rand();
    } while (UNLIKELY(r >= limit));

    return r % upper;
#endif
}

/**
 * Structure-aware memory allocator with automatic cleanup.
 *
 * Nalloc provides a hierarchical memory management system where each allocation
 * can have a parent dependency. When a parent is freed, all its children are
 * automatically freed as well.
 *
 * Key features:
 * - Automatic cleanup of dependent allocations
 * - Tree-structured memory dependencies
 * - Compatible replacement for malloc/calloc/realloc/free
 * - Zero overhead when used correctly
 *
 * Warning: Do not mix standard malloc/free with nalloc functions
 *          on the same memory chunk.
 *
 * Example usage:
 *   // Create a matrix with automatic cleanup
 *   matrix_t *m = nalloc(sizeof(matrix_t), NULL);
 *   m->rows = ncalloc(height, sizeof(int*), m);
 *   for (int i = 0; i < height; i++) {
 *       m->rows[i] = nalloc(width * sizeof(int), m->rows);
 *   }
 *   nfree(m);  // Frees everything automatically
 */

/**
 * Allocate memory with optional parent dependency.
 *
 * @size   : Number of bytes to allocate
 * @parent : Parent allocation, or NULL for root allocation
 * Return pointer to allocated memory, or NULL on failure
 */
void *nalloc(size_t size, void *parent);

/**
 * Allocate zero-initialized memory with optional parent dependency.
 *
 * @count  : Number of elements to allocate
 * @size   : Size of each element in bytes
 * @parent : Parent allocation, or NULL for root allocation
 * Return pointer to allocated memory, or NULL on failure
 */
void *ncalloc(size_t count, size_t size, void *parent);

/**
 * Resize an existing nalloc allocation.
 *
 * @ptr    : Previously allocated memory, or NULL
 * @size   : New size in bytes
 * Return pointer to resized memory, or NULL on failure
 *
 * @note : Parent relationships are preserved across realloc
 */
void *nrealloc(void *ptr, size_t size);

/**
 * Free memory and all dependent allocations.
 *
 * @ptr : Memory to free, or NULL (safe to call)
 * Return always NULL for convenience
 */
void *nfree(void *ptr);

/**
 * Change the parent of an existing allocation.
 *
 * This moves the entire subtree rooted at @ptr to become
 * a child of @parent.
 *
 * @ptr    : Memory whose parent to change
 * @parent : New parent, or NULL to make it a root
 */
void nalloc_set_parent(void *ptr, void *parent);

/**
 * Get allocation statistics (debug/profiling helper).
 *
 * @ptr   : Allocation to query
 * @stats : Structure to fill with statistics
 * Return 0 on success, -1 if ptr is invalid
 */
typedef struct {
    size_t total_size;  /* Total bytes including children */
    size_t direct_size; /* Direct allocation size */
    int child_count;    /* Number of direct children */
    int depth;          /* Depth in dependency tree */
} nalloc_stats_t;

int nalloc_get_stats(const void *ptr, nalloc_stats_t *stats);
