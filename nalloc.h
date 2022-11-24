/**
 * \brief A structure aware memory allocator.
 *
 * Nalloc is a nested replacement of for the standard memory allocation
 * functions, tracking the natural structure aware tree-like representation
 * of memory dependencies. Freeing a alloced chunk of memory would release
 * all of its dependencies.
 *
 * @note You can not mix malloc and nalloc families of functions for
 *       a given chunk of memory. Once a chunk is allocated with *nalloc*,
 *       it can only be freed with *nfree*.
 *
 * Use:
 * @code
 *   struct matrix { size_t rows, cols; int **data; };
 *   struct matrix *matrix_new(size_t rows, size_t cols) {
 *       struct matrix *m = ncalloc(sizeof(*m), NULL);
 *       m->rows = rows; m->cols = cols;
 *       m->data = ncalloc(rows * sizeof(*m->data), m);
 *       for (size_t i = 0; i < rows; i++)
 *           m->data[i] = nalloc(cols * sizeof(**m->data), m->data);
 *       return m;
 *   }
 *   void matrix_delete(struct matrix *m) { nfree(m); }
 * @endcode
 */

#pragma once

#include <stddef.h>

/**
 * Allocate a (contiguous) memory chunk.
 *
 * @param size    amount of memory requested (in bytes).
 * @param parent  pointer to allocated memory chunk from which this
 *                chunk depends, or NULL.
 *
 * @return pointer to the allocated memory chunk, or NULL if there was an error.
 */
void *nalloc(size_t size, void *parent);

/**
 * Allocate a zeroed (contiguous) memory chunk.
 *
 * @param count   amount of objects
 * @param size    amount of each memory object requested (in bytes).
 * @param parent  pointer to allocated memory chunk from which this
 *                chunk depends, or NULL.
 *
 * @return pointer to the allocated memory chunk, or NULL if there was an error.
 */
void *ncalloc(size_t count, size_t size, void *parent);

/**
 * Modify the size of a memory chunk.
 *
 * @param mem   pointer to allocated memory chunk.
 * @param size  amount of memory requested (in bytes).
 *
 * @return pointer to the allocated memory chunk.
 * @return NULL if there was an error.
 */
void *nrealloc(void *mem, size_t size);

/**
 * Deallocate a memory chunk and all the chunks depending on it.
 *
 * @param mem  pointer to allocated memory chunk.
 *
 * @return always NULL, can be safely ignored.
 */
void *nfree(void *mem);

/**
 * Change the parent of a memory chunk. This will affect the
 * dependencies of the entire subtree rooted at the given chunk.
 *
 * @param mem     pointer to allocated memory chunk.
 * @param parent  pointer to allocated memory chunk from which this
 *                chunk depends, or NULL.
 */
void nalloc_set_parent(void *mem, void *parent);
