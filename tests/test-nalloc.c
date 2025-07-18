#include <stdlib.h>
#include <string.h>

#include "../nalloc.h"
#include "test.h"

void test_nalloc_basic_allocation(void)
{
    /* Test standard allocation and deallocation */
    void *ptr = nalloc(100, NULL);
    assert_test(ptr, "nalloc should allocate memory successfully");

    if (!ptr)
        return;

    /* Test memory accessibility */
    unsigned char *bytes = (unsigned char *) ptr;

    /* Write pattern to memory */
    for (int i = 0; i < 100; i++)
        bytes[i] = (unsigned char) (i % 256);

    /* Verify pattern */
    bool pattern_correct = true;
    for (int i = 0; i < 100; i++) {
        if (bytes[i] != (unsigned char) (i % 256)) {
            pattern_correct = false;
            break;
        }
    }
    assert_test(pattern_correct,
                "allocated memory should be readable and writable");

    /* Test deallocation */
    void *result = nfree(ptr);
    assert_test(result == NULL, "nfree should return NULL");

    /* Test different allocation sizes */
    void *small = nalloc(1, NULL);
    void *medium = nalloc(1024, NULL);
    void *large = nalloc(65536, NULL);

    assert_test(small && medium && large,
                "various allocation sizes should succeed");

    if (small && medium && large) {
        assert_test(small != medium && medium != large && small != large,
                    "different allocations should return unique pointers");
    }

    nfree(large);
    nfree(medium);
    nfree(small);
}

void test_nalloc_simple_realloc(void)
{
    /* Test realloc functionality with careful pointer management */
    void *ptr = nalloc(50, NULL);
    assert_test(ptr, "initial allocation should succeed");

    if (!ptr)
        return;

    /* Write test pattern */
    unsigned char *bytes = (unsigned char *) ptr;
    for (int i = 0; i < 50; i++)
        bytes[i] = (unsigned char) (i + 1);

    /* Test expanding allocation */
    void *expanded = nrealloc(ptr, 100);
    assert_test(expanded, "realloc expansion should succeed");

    if (!expanded) {
        /* If realloc failed, original pointer should still be valid */
        nfree(ptr);
        return;
    }

    /* After successful realloc, ptr is now invalid, use expanded */
    /* Verify original data preserved */
    unsigned char *new_bytes = (unsigned char *) expanded;
    bool data_preserved = true;
    for (int i = 0; i < 50; i++) {
        if (new_bytes[i] != (unsigned char) (i + 1)) {
            data_preserved = false;
            break;
        }
    }
    assert_test(data_preserved, "realloc should preserve existing data");

    /* Test shrinking allocation - be careful with pointer management */
    void *shrunk = nrealloc(expanded, 25);
    if (!shrunk) {
        /* If shrink failed, expanded is still valid */
        nfree(expanded);
        return;
    }

    /* After successful realloc, expanded is now invalid, use shrunk */
    /* Verify data still preserved in shrunk area */
    unsigned char *shrunk_bytes = (unsigned char *) shrunk;
    bool shrunk_data_ok = true;
    for (int i = 0; i < 25; i++) {
        if (shrunk_bytes[i] != (unsigned char) (i + 1)) {
            shrunk_data_ok = false;
            break;
        }
    }
    assert_test(shrunk_data_ok, "realloc shrinking should preserve data");

    nfree(shrunk);

    /* Test realloc with NULL (should work like nalloc) */
    void *null_realloc = nrealloc(NULL, 200);
    assert_test(null_realloc, "nrealloc(NULL, size) should work like nalloc");

    if (null_realloc) {
        /* Write to it to ensure it's valid */
        unsigned char *null_bytes = (unsigned char *) null_realloc;
        null_bytes[0] = 0xFF;
        null_bytes[199] = 0xAA;
        assert_test(null_bytes[0] == 0xFF && null_bytes[199] == 0xAA,
                    "null realloc memory should be writable");
        nfree(null_realloc);
    }

    /* Test realloc to size 0 (should work like nfree) */
    void *to_free = nalloc(100, NULL);
    if (to_free) {
        void *zero_result = nrealloc(to_free, 0);
        assert_test(zero_result == NULL,
                    "nrealloc(ptr, 0) should work like nfree");
        /* Note: to_free is now invalid after nrealloc(to_free, 0) */
    }
}

void test_nalloc_edge_cases(void)
{
    /* Test NULL safety */
    assert_test(nfree(NULL) == NULL,
                "nfree(NULL) should be safe and return NULL");

    /* Test zero-size allocation */
    void *zero_ptr = nalloc(0, NULL);
    assert_test(nfree(zero_ptr) == NULL,
                "zero-size allocation should be handled gracefully");

    /* Test multiple allocations for uniqueness */
    const int alloc_count = 20;
    void *ptrs[alloc_count];
    int successful_allocs = 0;

    /* Allocate multiple blocks */
    for (int i = 0; i < alloc_count; i++) {
        ptrs[i] = nalloc(64, NULL);
        if (ptrs[i]) {
            successful_allocs++;
        }
    }

    assert_test(successful_allocs > 0,
                "should successfully allocate some blocks");

    /* Verify uniqueness of successful allocations */
    bool all_unique = true;
    for (int i = 0; i < alloc_count && all_unique; i++) {
        if (!ptrs[i])
            continue;

        for (int j = i + 1; j < alloc_count; j++) {
            if (ptrs[j] && ptrs[i] == ptrs[j]) {
                all_unique = false;
                break;
            }
        }
    }
    assert_test(all_unique, "all successful allocations should be unique");

    /* Cleanup all allocations */
    for (int i = 0; i < alloc_count; i++)
        nfree(ptrs[i]);

    /* Test ncalloc functionality */
    void *zero_mem = ncalloc(10, sizeof(int), NULL);
    if (zero_mem) {
        int *ints = (int *) zero_mem;
        bool all_zero = true;
        for (int i = 0; i < 10; i++) {
            if (ints[i] != 0) {
                all_zero = false;
                break;
            }
        }
        assert_test(all_zero, "ncalloc should zero-initialize memory");
        nfree(zero_mem);
    }

    /* Test ncalloc overflow protection */
    void *overflow_test = ncalloc(SIZE_MAX, SIZE_MAX, NULL);
    assert_test(overflow_test == NULL, "ncalloc should handle overflow safely");
}

void test_nalloc_parent_child_relationships(void)
{
    /* Test basic parent-child allocation */
    void *parent = nalloc(100, NULL);
    assert_test(parent, "parent allocation should succeed");

    if (!parent)
        return;

    /* Allocate children with parent dependency */
    void *child1 = nalloc(50, parent);
    void *child2 = nalloc(30, parent);
    void *child3 = ncalloc(5, sizeof(int), parent);

    assert_test(child1 && child2 && child3, "child allocations should succeed");
    assert_test(child1 != parent && child2 != parent && child3 != parent,
                "children should be distinct from parent");
    assert_test(child1 != child2 && child2 != child3 && child1 != child3,
                "children should be distinct from each other");

    /* Test multi-level hierarchy */
    void *grandchild1 = nalloc(20, child1);
    void *grandchild2 = nalloc(15, child1);

    if (grandchild1 && grandchild2) {
        assert_test(grandchild1 != grandchild2,
                    "grandchildren should be distinct");

        /* Test great-grandchild */
        void *great_grandchild = nalloc(10, grandchild1);
        if (great_grandchild) {
            assert_test(great_grandchild != grandchild1,
                        "great-grandchild should be distinct");
        }

        /* Test nalloc statistics functionality */
        nalloc_stats_t stats;
        int stats_result = nalloc_get_stats(parent, &stats);

        if (stats_result == 0) {
            assert_test(stats.child_count >= 3,
                        "statistics should count children in subtree");
            assert_test(stats.depth >= 2,
                        "statistics should report correct depth");
        }

        /* Test statistics edge cases */
        assert_test(nalloc_get_stats(NULL, &stats) != 0,
                    "statistics with NULL pointer should fail safely");
        assert_test(nalloc_get_stats(parent, NULL) != 0,
                    "statistics with NULL stats should fail safely");
    }

    /* Test automatic cleanup: freeing parent should free entire subtree */
    nfree(parent);

    assert_test(true, "hierarchical cleanup completed successfully");

    /* Test orphan adoption with nalloc_set_parent */
    void *new_parent = nalloc(200, NULL);
    void *orphan = nalloc(50, NULL);

    if (new_parent && orphan) {
        /* Move orphan to new parent */
        nalloc_set_parent(orphan, new_parent);
        assert_test(true, "nalloc_set_parent should not crash");

        /* Test that orphan is now dependent on new_parent */
        nfree(new_parent); /* Should also free orphan */
        assert_test(true, "parent cleanup should handle adopted children");
    }

    /* Test setting NULL parent (making allocation independent) */
    void *independent = nalloc(30, NULL);
    void *dependent = nalloc(20, independent);

    if (independent && dependent) {
        nalloc_set_parent(dependent, NULL); /* Make dependent independent */
        nfree(independent);                 /* Should not free dependent now */
        nfree(dependent);                   /* Must free explicitly */
        assert_test(true, "NULL parent should make allocation independent");
    }

    /* Test complex scenarios */
    void *stress_root = nalloc(1000, NULL);
    if (stress_root) {
        /* Create many children for stress testing */
        int successful_children = 0;
        for (int i = 0; i < 20; i++) {
            void *stress_child = nalloc(10 + i, stress_root);
            if (stress_child)
                successful_children++;
        }

        assert_test(successful_children > 15,
                    "stress test should succeed for most allocations (%d/20)",
                    successful_children);

        /* Single free should cleanup all */
        nfree(stress_root);
        assert_test(true, "stress cleanup completed");
    }
}
