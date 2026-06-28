/*
 * test_arena — pins the single-sourced cart-heap arena allocator
 * (runtime/shared/blyt_arena.c, ADR-0008 #158, issue #158).
 *
 * This is the ONE allocator the cart heap runs through on every leg (emulated
 * libblytc, native libblytc, the WASM host-Lua fast path), so `guest_heap_used`
 * is identical by construction across platforms (ADR-0007). This test pins:
 *   - the first-fit / split / forward-coalesce behaviour (ported from the
 *     original blytc_arena, so the refactor is behaviour-preserving),
 *   - the unified-budget accounting: guest_heap_used tracks live bytes and the
 *     16 MB logical cap (blyt_mem_budget.h) is enforced *before* the physical
 *     arena fills, using the host-published non_evictable_footprint,
 *   - reset() (hot-swap, #133) restoring a fresh-load-identical state.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "blyt_arena.h"
#include "blyt_mem_budget.h"

/* A modest physical arena; the 16 MB *logical* cap is exercised via footprint,
 * so we never need a 16 MB buffer to test cap enforcement. */
static uint8_t g_buf[8192] __attribute__((aligned(16)));

static blyt_arena_t make_arena(blyt_mem_accounting_t *acct) {
    blyt_arena_t a = {0};
    a.base = g_buf;
    a.size = sizeof(g_buf);
    a.acct = acct;
    return a;
}

static void test_basic_alloc_in_bounds(void) {
    blyt_mem_accounting_t acct = {0};
    blyt_arena_t a = make_arena(&acct);

    void *p = blyt_arena_malloc(&a, 100);
    assert(p);
    assert((uint8_t *)p >= g_buf && (uint8_t *)p + 100 <= g_buf + sizeof(g_buf));
    /* 16-byte aligned user data (largest natural alignment on RV32IMAFDC). */
    assert(((uintptr_t)p & 15u) == 0);
    memset(p, 0xAB, 100); /* writable */
    /* Accounting moved off zero by at least the request. */
    assert(acct.guest_heap_used >= 100);
    blyt_arena_free(&a, p);
    assert(acct.guest_heap_used == 0); /* freeing the only block returns to 0 */
}

static void test_accounting_tracks_live_bytes(void) {
    blyt_mem_accounting_t acct = {0};
    blyt_arena_t a = make_arena(&acct);

    void *p = blyt_arena_malloc(&a, 64);
    uint32_t after_p = acct.guest_heap_used;
    assert(after_p >= 64);
    void *q = blyt_arena_malloc(&a, 64);
    assert(acct.guest_heap_used > after_p);
    blyt_arena_free(&a, q);
    assert(acct.guest_heap_used == after_p); /* back to just p */

    /* Free p, re-malloc the same size: first-fit reuse, same live total. */
    blyt_arena_free(&a, p);
    assert(acct.guest_heap_used == 0);
    void *r = blyt_arena_malloc(&a, 64);
    assert(acct.guest_heap_used == after_p); /* reused the coalesced front block */
    blyt_arena_free(&a, r);
}

static void test_calloc_zeroes_and_realloc_grows(void) {
    blyt_mem_accounting_t acct = {0};
    blyt_arena_t a = make_arena(&acct);

    uint8_t *c = blyt_arena_calloc(&a, 10, 4);
    assert(c);
    for (int i = 0; i < 40; i++)
        assert(c[i] == 0);
    memcpy(c, "hello", 5);

    uint8_t *g = blyt_arena_realloc(&a, c, 4096);
    assert(g);
    assert(memcmp(g, "hello", 5) == 0); /* data preserved across grow */
    blyt_arena_free(&a, g);
    assert(acct.guest_heap_used == 0);
}

static void test_logical_cap_before_physical(void) {
    blyt_mem_accounting_t acct = {0};
    blyt_arena_t a = make_arena(&acct);

    /* Pin the non-evictable footprint just 100 bytes below the 16 MB cap, so the
     * unified budget allows < 100 bytes of heap even though the physical arena
     * has 8 KB free. The first small alloc fits; the next must fail on the
     * LOGICAL cap, not because the arena is full. */
    acct.non_evictable_footprint = BLYT_MEM_BUDGET_BYTES - 100u;

    void *p = blyt_arena_malloc(&a, 1); /* tiny: header+align ~ <= 100 */
    assert(p);
    assert(acct.guest_heap_used <= 100);

    void *q = blyt_arena_malloc(&a, 100); /* would push past 16 MB */
    assert(q == NULL); /* failed on the unified cap */
    /* The failed allocation must not have perturbed accounting. */
    assert(acct.guest_heap_used <= 100);

    /* Relieve footprint pressure → the same allocation now succeeds (no eviction
     * needed here; the arena just re-checks the predicate). */
    acct.non_evictable_footprint = 0;
    void *r = blyt_arena_malloc(&a, 100);
    assert(r);
    blyt_arena_free(&a, p);
    blyt_arena_free(&a, r);
    assert(acct.guest_heap_used == 0);
}

static void test_null_acct_is_uncapped(void) {
    /* A NULL accounting pointer = unaccounted/uncapped (e.g. a transitional or
     * test arena): allocation is bounded only by the physical arena. */
    blyt_arena_t a = make_arena(NULL);
    void *p = blyt_arena_malloc(&a, 100);
    assert(p);
    blyt_arena_free(&a, p);
}

static void test_reset_restores_fresh_state(void) {
    blyt_mem_accounting_t acct = {0};
    blyt_arena_t a = make_arena(&acct);

    void *p = blyt_arena_malloc(&a, 200);
    assert(p);
    assert(acct.guest_heap_used > 0);

    /* Hot-swap reset (#133): fresh bump + empty free list + zeroed heap total,
     * so a reloaded cart's first allocation is bit-identical to a fresh load. */
    blyt_arena_reset(&a);
    assert(acct.guest_heap_used == 0);

    void *q = blyt_arena_malloc(&a, 200);
    assert(q == p); /* allocates from the base again, identical address */
    blyt_arena_free(&a, q);
}

int main(void) {
    test_basic_alloc_in_bounds();
    test_accounting_tracks_live_bytes();
    test_calloc_zeroes_and_realloc_grows();
    test_logical_cap_before_physical();
    test_null_acct_is_uncapped();
    test_reset_restores_fresh_state();
    printf("test_arena: all passed\n");
    return 0;
}
