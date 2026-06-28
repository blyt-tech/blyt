/*
 * test_resource_budget — host resource-side budget accounting + LRU-incremental
 * eviction policy (runtime/host/src/libblyt/resource.c, ADR-0008/0027 #158).
 *
 * #137 landed the eviction *mechanism* (per-entry evict + the all-at-once sweep);
 * #158 adds the *policy* this test pins:
 *   - non_evictable_footprint = Sum e->len over loaded/pinned entries (the
 *     determinism-bearing figure the host publishes to the guest),
 *   - resident_evictable = Sum e->len over evictable entries with owned bytes
 *     (the advisory reclaimable cache),
 *   - a recency stamp (touch) + LRU-incremental evict_to_fit that frees the
 *     MINIMUM, least-recently-used first, stopping the instant it fits (AC6).
 */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "blyt_resource_codec.h" /* BLYT_RES_ALGO_* */
#include "resource.h"

static const char PAYLOAD[] =
    "hello, blyt resource codec! hello, blyt resource codec! hello, blyt resource codec!";
static const uint8_t FRAME[] = {0x28, 0xb5, 0x2f, 0xfd, 0x24, 0x53, 0x25, 0x01, 0x00, 0xe8,
                                0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x2c, 0x20, 0x62, 0x6c, 0x79,
                                0x74, 0x20, 0x72, 0x65, 0x73, 0x6f, 0x75, 0x72, 0x63, 0x65,
                                0x20, 0x63, 0x6f, 0x64, 0x65, 0x63, 0x21, 0x20, 0x68, 0x01,
                                0x00, 0xed, 0xcf, 0xe9, 0x04, 0x41, 0xf9, 0x41, 0x97};

#define L (sizeof(PAYLOAD) - 1) /* 83-byte decompressed payload */

static blyt_resource_entry_t *push_zstd(blyt_resource_table_t *t, uint32_t id) {
    blyt_resource_entry_t *e = blyt_resource_table_test_push(t);
    assert(e);
    e->id = id;
    e->algo = BLYT_RES_ALGO_ZSTD;
    e->cdata = FRAME;
    e->clen = sizeof(FRAME);
    e->len = L;
    e->data = NULL;
    e->owned = NULL;
    return e;
}

static void test_footprint_counts_loaded_and_pinned(void) {
    blyt_resource_table_t t;
    blyt_resource_table_init(&t);
    for (uint32_t id = 1; id <= 4; id++)
        push_zstd(&t, id);

    /* Nothing loaded/pinned: footprint is zero (even if some are resident). */
    assert(blyt_resource_entry_data(blyt_resource_table_find_mut(&t, 1)) != NULL);
    assert(blyt_resource_table_footprint(&t) == 0);

    /* Footprint counts e->len up front — a load reserves it even before any
     * decompress (e->len is authoritative pre-decode, #157). */
    uint32_t h2 = blyt_rl_load(&blyt_resource_table_find_mut(&t, 2)->rl, 2);
    assert(blyt_resource_table_footprint(&t) == L); /* #2 loaded, not yet decoded */

    blyt_rl_pin(&blyt_resource_table_find_mut(&t, 3)->rl);
    assert(blyt_resource_table_footprint(&t) == 2 * L); /* #2 load + #3 pin */

    /* Releasing/unpinning shrinks the footprint back. */
    assert(blyt_rl_release(&blyt_resource_table_find_mut(&t, 2)->rl, h2) == 1);
    assert(blyt_resource_table_footprint(&t) == L);
    assert(blyt_rl_unpin(&blyt_resource_table_find_mut(&t, 3)->rl) == 1);
    assert(blyt_resource_table_footprint(&t) == 0);
    blyt_resource_table_clear(&t);
}

static void test_resident_evictable_excludes_referenced(void) {
    blyt_resource_table_t t;
    blyt_resource_table_init(&t);
    for (uint32_t id = 1; id <= 3; id++) {
        blyt_resource_entry_t *e = push_zstd(&t, id);
        assert(blyt_resource_entry_data(e) != NULL); /* all resident */
    }
    assert(blyt_resource_table_resident_evictable(&t) == 3 * L);

    /* A load makes #2 non-evictable: it drops out of the evictable cache total
     * (it is now part of the footprint instead). */
    blyt_rl_load(&blyt_resource_table_find_mut(&t, 2)->rl, 2);
    assert(blyt_resource_table_resident_evictable(&t) == 2 * L);
    assert(blyt_resource_table_footprint(&t) == L);
    blyt_resource_table_clear(&t);
}

static void test_evict_to_fit_is_lru_incremental(void) {
    blyt_resource_table_t t;
    blyt_resource_table_init(&t);
    for (uint32_t id = 1; id <= 3; id++) {
        blyt_resource_entry_t *e = push_zstd(&t, id);
        assert(blyt_resource_entry_data(e) != NULL);
    }
    /* Establish recency: touch 1, then 2, then 3 — so #1 is least-recently-used,
     * #3 most-recently. */
    blyt_resource_table_touch(&t, blyt_resource_table_find_mut(&t, 1));
    blyt_resource_table_touch(&t, blyt_resource_table_find_mut(&t, 2));
    blyt_resource_table_touch(&t, blyt_resource_table_find_mut(&t, 3));
    assert(blyt_resource_table_resident_evictable(&t) == 3 * L);

    /* Fit within 2*L of resident cache: must free exactly ONE — the LRU (#1) —
     * and leave the other two resident (the minimum-needed, AC6). */
    size_t freed = blyt_resource_table_evict_to_fit(&t, 2 * L);
    assert(freed == L);
    assert(blyt_resource_table_find_mut(&t, 1)->owned == NULL); /* LRU victim */
    assert(blyt_resource_table_find_mut(&t, 2)->owned != NULL);
    assert(blyt_resource_table_find_mut(&t, 3)->owned != NULL);
    assert(blyt_resource_table_resident_evictable(&t) == 2 * L);

    /* Already fits: a no-op. */
    assert(blyt_resource_table_evict_to_fit(&t, 2 * L) == 0);

    /* Target 0: evict the rest, LRU-first (#2 before #3). */
    assert(blyt_resource_table_evict_to_fit(&t, 0) == 2 * L);
    assert(blyt_resource_table_resident_evictable(&t) == 0);
    blyt_resource_table_clear(&t);
}

static void test_evict_to_fit_skips_referenced(void) {
    blyt_resource_table_t t;
    blyt_resource_table_init(&t);
    for (uint32_t id = 1; id <= 2; id++) {
        blyt_resource_entry_t *e = push_zstd(&t, id);
        assert(blyt_resource_entry_data(e) != NULL);
    }
    /* #1 is the LRU but it is pinned → not evictable; evict_to_fit must skip it
     * and take #2 instead, never sacrificing a referenced entry. */
    blyt_resource_table_touch(&t, blyt_resource_table_find_mut(&t, 1));
    blyt_resource_table_touch(&t, blyt_resource_table_find_mut(&t, 2));
    blyt_rl_pin(&blyt_resource_table_find_mut(&t, 1)->rl);

    size_t freed = blyt_resource_table_evict_to_fit(&t, 0);
    assert(freed == L); /* only #2: #1 is pinned */
    assert(blyt_resource_table_find_mut(&t, 1)->owned != NULL); /* pinned, intact */
    assert(blyt_resource_table_find_mut(&t, 2)->owned == NULL);
    blyt_resource_table_clear(&t);
}

int main(void) {
    test_footprint_counts_loaded_and_pinned();
    test_resident_evictable_excludes_referenced();
    test_evict_to_fit_is_lru_incremental();
    test_evict_to_fit_skips_referenced();
    printf("test_resource_budget: all passed\n");
    return 0;
}
