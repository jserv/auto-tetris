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

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "utils.h"

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

/* Forward declaration for tree traversal helper */
static void update_refs(void *node, void *old_ptr, void *new_ptr);

/* Find root node by traversing up the tree */
static void *find_tree_root(void *node)
{
    if (UNLIKELY(!node))
        return NULL;

    while (!is_root(node)) {
        void *parent = get_parent(node);
        if (UNLIKELY(!parent))
            break;
        node = parent;
    }
    return node;
}

/* Recursively update all references to old_ptr with new_ptr in subtree */
static void update_refs(void *node, void *old_ptr, void *new_ptr)
{
    if (UNLIKELY(!node))
        return;

    nalloc_header_t *header = get_header(node);

    /* Update pointers in this node's header */
    if (header->first_child == old_ptr)
        header->first_child = new_ptr;
    if (header->next_sibling == old_ptr)
        header->next_sibling = new_ptr;
    if (header->prev_sibling == old_ptr)
        header->prev_sibling = new_ptr;

    /* Recursively update all children */
    void *child = header->first_child;
    while (child) {
        void *next_child = next_sibling(child);
        update_refs(child, old_ptr, new_ptr);
        child = next_child;
    }
}

void *nrealloc(void *ptr, size_t size)
{
    if (UNLIKELY(size == 0)) {
        nfree(ptr);
        return NULL;
    }

    if (UNLIKELY(!ptr))
        return nalloc(size, NULL);

    /* Save complete state before realloc. Must capture everything while
     * pointers are still valid.
     */
    nalloc_header_t saved_header = *get_header(ptr);
    void *parent = get_parent(ptr);
    bool was_first_child = is_first_child(ptr);
    bool was_root = is_root(ptr);

    /* Find tree root while tree structure is intact */
    void *tree_root = find_tree_root(ptr);

    void *old_raw = user_to_raw(ptr);
    size_t total_size = ALIGN_SIZE(HEADER_SIZE + size);
    void *new_raw = realloc(old_raw, total_size);

    if (UNLIKELY(!new_raw))
        return NULL;

    void *new_ptr = raw_to_user(new_raw);

    /* If address changed, update all references */
    if (LIKELY(new_ptr != ptr)) {
        /* Restore header data */
        *get_header(new_ptr) = saved_header;

        /* Update parent's first_child pointer if needed */
        if (parent && first_child(parent) == ptr)
            first_child(parent) = new_ptr;

        /* Update next sibling's prev_sibling pointer */
        if (saved_header.next_sibling)
            prev_sibling(saved_header.next_sibling) = new_ptr;

        /* Update previous sibling's next_sibling pointer if not first child */
        if (!was_root && !was_first_child && saved_header.prev_sibling)
            next_sibling(saved_header.prev_sibling) = new_ptr;

        /* Update all children's parent pointers if we have children */
        if (saved_header.first_child) {
            /* For first child, prev_sibling points to parent */
            prev_sibling(saved_header.first_child) = new_ptr;
        }

        /* Recursively update any deeper references in the tree.
         * Adjust tree_root if we moved the root itself.
         */
        if (tree_root == ptr)
            tree_root = new_ptr;

        if (tree_root)
            update_refs(tree_root, ptr, new_ptr);
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
        /* We are first child, update parent's first_child pointer */
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
