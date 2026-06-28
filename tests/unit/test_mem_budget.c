/*
 * test_mem_budget — pins the shared unified-memory-budget accounting primitives
 * (runtime/shared/blyt_mem_budget.h, ADR-0008 #158 amendment, issue #158).
 *
 * Pure logic, freestanding (stdint only): the 16 MB cap constant, the
 * overflow-safe "does this allocation fit the unified budget" predicate, and the
 * "bytes left for the evictable resource cache" helper. The module compiles into
 * the host runtime, the rv32 guest libs, AND the WASM host-Lua fast path, so the
 * success/failure decision is identical on every leg — this test pins that
 * decision once, here.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "blyt_mem_budget.h"

static void test_budget_constant(void) {
    /* The cart-visible working-memory cap is exactly 16 MiB (ADR-0008). */
    assert(BLYT_MEM_BUDGET_BYTES == 16u * 1024u * 1024u);
}

static void test_alloc_fits_basic(void) {
    /* Empty: a full-budget allocation fits exactly; one byte more does not. */
    assert(blyt_mem_alloc_fits(0, 0, BLYT_MEM_BUDGET_BYTES));
    assert(!blyt_mem_alloc_fits(0, 0, BLYT_MEM_BUDGET_BYTES + 1u));

    /* The predicate is the *sum* of all three terms vs the cap. */
    assert(blyt_mem_alloc_fits(4u << 20, 4u << 20, 8u << 20)); /* 4+4+8 = 16 */
    assert(!blyt_mem_alloc_fits(4u << 20, 4u << 20, (8u << 20) + 1u));
}

static void test_alloc_fits_unified_pools(void) {
    /* Heap and the non-evictable footprint draw on the SAME 16 MB, not two
     * separate pools: a 10 MB footprint leaves only 6 MB for the heap. */
    assert(blyt_mem_alloc_fits(0, 10u << 20, 6u << 20));
    assert(!blyt_mem_alloc_fits(0, 10u << 20, (6u << 20) + 1u));
    /* Symmetric: a 10 MB heap leaves only 6 MB of footprint headroom. */
    assert(blyt_mem_alloc_fits(10u << 20, 0, 6u << 20));
    assert(!blyt_mem_alloc_fits(10u << 20, 6u << 20, 1u));
}

static void test_alloc_fits_overflow_safe(void) {
    /* Near-UINT32_MAX terms must not wrap to a false "fits". */
    assert(!blyt_mem_alloc_fits(0xFFFFFFFFu, 0xFFFFFFFFu, 1u));
    assert(!blyt_mem_alloc_fits(0, 0, 0xFFFFFFFFu));
    /* Zero-byte allocation always fits when already at the cap (no-op). */
    assert(blyt_mem_alloc_fits(BLYT_MEM_BUDGET_BYTES, 0, 0));
}

static void test_cache_room(void) {
    /* Room for the evictable cache = budget - heap - non-evictable footprint. */
    assert(blyt_mem_cache_room(0, 0) == BLYT_MEM_BUDGET_BYTES);
    assert(blyt_mem_cache_room(4u << 20, 2u << 20) == (10u << 20));
    /* Exactly full: zero room, not underflow. */
    assert(blyt_mem_cache_room(8u << 20, 8u << 20) == 0u);
    /* Over-committed (shouldn't happen, but must clamp, never wrap). */
    assert(blyt_mem_cache_room(12u << 20, 8u << 20) == 0u);
    assert(blyt_mem_cache_room(0xFFFFFFFFu, 0xFFFFFFFFu) == 0u);
}

int main(void) {
    test_budget_constant();
    test_alloc_fits_basic();
    test_alloc_fits_unified_pools();
    test_alloc_fits_overflow_safe();
    test_cache_room();
    printf("test_mem_budget: all passed\n");
    return 0;
}
