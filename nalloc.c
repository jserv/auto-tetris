/*
 * Structure-aware memory allocator implementation.
 *
 * Memory layout per allocation:
 * +---------+---------+---------+--------+--------···
 * |  first  |  next   |  prev   | size   | user
 * |  child  | sibling | sibling | (opt)  | data
 * +---------+---------+---------+--------+--------···
 *
 * Tree structure relationships:
 * - Each node can have multiple children (linked list via next/prev)
 * - Children are organized as doubly-linked sibling lists
 * - Parent reference is implicit (stored in prev when is_first_child)
 */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "nalloc.h"

/* Compiler hints for better optimization */
#if defined(__GNUC__) || defined(__clang__)
#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define LIKELY(x) (x)
#define UNLIKELY(x) (x)
#endif

/* Memory alignment for better performance */
#define ALIGNMENT sizeof(void *)
#define ALIGN_SIZE(size) (((size) + ALIGNMENT - 1) & ~(ALIGNMENT - 1))

/* Header layout */
typedef struct nalloc_header {
    void *first_child;  /* First child in the dependency tree */
    void *next_sibling; /* Next sibling in parent's child list */
    void *prev_sibling; /* Previous sibling or parent if first child */
} nalloc_header_t;

#define HEADER_SIZE (sizeof(nalloc_header_t))

/* Pointer arithmetic macros */
#define raw_to_user(raw) ((char *) (raw) + HEADER_SIZE)
#define user_to_raw(user) ((char *) (user) - HEADER_SIZE)
#define get_header(user) ((nalloc_header_t *) user_to_raw(user))

/* Tree navigation helpers */
#define first_child(ptr) (get_header(ptr)->first_child)
#define next_sibling(ptr) (get_header(ptr)->next_sibling)
#define prev_sibling(ptr) (get_header(ptr)->prev_sibling)

/* Tree state queries */
#define is_root(ptr) (prev_sibling(ptr) == NULL)
#define is_first_child(ptr) \
    (prev_sibling(ptr) != NULL && next_sibling(prev_sibling(ptr)) != (ptr))
#define get_parent(ptr) (is_first_child(ptr) ? prev_sibling(ptr) : NULL)

/* Initialize header for a newly allocated chunk. */
static inline void *init_allocation(void *raw_mem, void *parent)
{
    if (UNLIKELY(!raw_mem))
        return NULL;

    /* Clear header */
    nalloc_header_t *header = (nalloc_header_t *) raw_mem;
    memset(header, 0, HEADER_SIZE);

    void *user_ptr = raw_to_user(raw_mem);

    /* Set up parent relationship if specified */
    if (parent)
        nalloc_set_parent(user_ptr, parent);

    return user_ptr;
}

/* Allocate raw memory with proper alignment. */
static inline void *allocate_raw(size_t user_size)
{
    size_t total_size = ALIGN_SIZE(HEADER_SIZE + user_size);
    return malloc(total_size);
}

/* Allocate zero-initialized raw memory with proper alignment. */
static inline void *callocate_raw(size_t user_size)
{
    size_t total_size = ALIGN_SIZE(HEADER_SIZE + user_size);
    return calloc(1, total_size);
}

void *nalloc(size_t size, void *parent)
{
    if (UNLIKELY(size == 0))
        return NULL;

    void *raw_mem = allocate_raw(size);
    return init_allocation(raw_mem, parent);
}

void *ncalloc(size_t count, size_t size, void *parent)
{
    if (UNLIKELY(count == 0 || size == 0))
        return NULL;

    /* Check for overflow */
    if (UNLIKELY(count > SIZE_MAX / size))
        return NULL;

    size_t total_user_size = count * size;
    void *raw_mem = callocate_raw(total_user_size);
    return init_allocation(raw_mem, parent);
}

void *nrealloc(void *ptr, size_t size)
{
    if (UNLIKELY(size == 0)) {
        nfree(ptr);
        return NULL;
    }

    if (UNLIKELY(!ptr))
        return nalloc(size, NULL);

    void *old_raw = user_to_raw(ptr);
    size_t total_size = ALIGN_SIZE(HEADER_SIZE + size);
    void *new_raw = realloc(old_raw, total_size);

    if (UNLIKELY(!new_raw))
        return NULL;

    void *new_ptr = raw_to_user(new_raw);

    /* Update all references if address changed */
    if (LIKELY(new_ptr != ptr)) {
        /* Update parent's reference to us */
        void *parent = get_parent(ptr);
        if (parent && first_child(parent) == ptr)
            first_child(parent) = new_ptr;

        /* Update siblings' references to us */
        if (!is_root(ptr)) {
            if (next_sibling(ptr))
                prev_sibling(next_sibling(ptr)) = new_ptr;

            if (!is_first_child(ptr))
                next_sibling(prev_sibling(ptr)) = new_ptr;
        }

        /* Update children's parent references */
        if (first_child(ptr))
            prev_sibling(first_child(ptr)) = new_ptr;
    }

    return new_ptr;
}

/* Remove node from sibling list without freeing. */
static inline void unlink_from_siblings(void *ptr)
{
    if (is_root(ptr))
        return;

    /* Update next sibling's prev pointer */
    if (next_sibling(ptr))
        prev_sibling(next_sibling(ptr)) = prev_sibling(ptr);

    /* Update previous element's next pointer */
    if (is_first_child(ptr)) {
        /* We're first child, update parent's first_child pointer */
        void *parent = get_parent(ptr);
        if (LIKELY(parent))
            first_child(parent) = next_sibling(ptr);
    } else {
        /* We're not first child, update previous sibling's next pointer */
        next_sibling(prev_sibling(ptr)) = next_sibling(ptr);
    }

    /* Clear our pointers */
    next_sibling(ptr) = NULL;
    prev_sibling(ptr) = NULL;
}

/* Recursively free all children of a node. */
static void free_children_recursive(void *ptr)
{
    if (UNLIKELY(!ptr))
        return;

    void *child = first_child(ptr);
    while (child) {
        void *next_child = next_sibling(child);

        /* Recursively free this child's subtree */
        free_children_recursive(child);
        free(user_to_raw(child));

        child = next_child;
    }

    /* Clear the first_child pointer */
    first_child(ptr) = NULL;
}

void *nfree(void *ptr)
{
    if (UNLIKELY(!ptr))
        return NULL;

    /* Remove from parent's child list */
    unlink_from_siblings(ptr);

    /* Free all children recursively */
    free_children_recursive(ptr);

    /* Free the node itself */
    free(user_to_raw(ptr));

    return NULL;
}

void nalloc_set_parent(void *ptr, void *parent)
{
    if (UNLIKELY(!ptr))
        return;

    /* Remove from current parent */
    unlink_from_siblings(ptr);

    /* Add to new parent if specified */
    if (parent) {
        /* Insert as first child */
        void *old_first = first_child(parent);
        if (old_first)
            prev_sibling(old_first) = ptr;

        next_sibling(ptr) = old_first;
        prev_sibling(ptr) = parent;
        first_child(parent) = ptr;
    }
}

/* Calculate subtree statistics recursively. */
static void calculate_stats_recursive(const void *ptr,
                                      nalloc_stats_t *stats,
                                      int depth)
{
    if (!ptr)
        return;

    stats->child_count++;
    if (depth > stats->depth)
        stats->depth = depth;

    /* Visit all children */
    const void *child = first_child(ptr);
    while (child) {
        calculate_stats_recursive(child, stats, depth + 1);
        child = next_sibling(child);
    }
}

int nalloc_get_stats(const void *ptr, nalloc_stats_t *stats)
{
    if (UNLIKELY(!ptr || !stats))
        return -1;

    memset(stats, 0, sizeof(*stats));

    /* Calculate statistics for this subtree */
    calculate_stats_recursive(ptr, stats, 0);

    /* Note: We can't easily get allocation sizes without storing them, so
     * direct_size and total_size remain 0 in this implementation.
     */

    return 0;
}
