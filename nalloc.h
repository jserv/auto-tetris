/**
 * \brief Structure-aware memory allocator with automatic cleanup.
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

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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

#ifdef __cplusplus
}
#endif
