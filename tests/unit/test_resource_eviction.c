/*
 * test_resource_eviction — the host resource eviction mechanism + rehydration
 * (runtime/host/src/libblyt/resource.c, ADR-0027 v2, #137).
 *
 * Eviction reclaims an entry's owned/decompressed bytes when nothing references
 * it (load_count==0 && pin_count==0, not persistent), re-pointing the entry to
 * its not-resident state; the next access re-materialises byte-identical bytes
 * from the cart section (zstd re-decode) or the dev-staging file. This pins the
 * mechanism directly: a zstd entry decodes, is evicted (owned freed), then
 * rehydrates to the identical bytes; a loaded or pinned entry is never evicted.
 *
 * The end-to-end cross-leg byte-identity (native/WASM/libretro) lives in the
 * integration suite (assets.rs); this is the cheap, deterministic host oracle.
 */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "blyt_resource_codec.h" /* BLYT_RES_ALGO_* */
#include "resource.h"

/* An 83-byte payload and its zstd frame (zstd -19), shared with
 * test_resource_codec.c — any valid frame decodes deterministically. */
static const char PAYLOAD[] =
    "hello, blyt resource codec! hello, blyt resource codec! hello, blyt resource codec!";
static const uint8_t FRAME[] = {0x28, 0xb5, 0x2f, 0xfd, 0x24, 0x53, 0x25, 0x01, 0x00, 0xe8,
                                0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x2c, 0x20, 0x62, 0x6c, 0x79,
                                0x74, 0x20, 0x72, 0x65, 0x73, 0x6f, 0x75, 0x72, 0x63, 0x65,
                                0x20, 0x63, 0x6f, 0x64, 0x65, 0x63, 0x21, 0x20, 0x68, 0x01,
                                0x00, 0xed, 0xcf, 0xe9, 0x04, 0x41, 0xf9, 0x41, 0x97};

/* Build a one-entry table holding a lazily-decoded zstd resource. The table has
 * no public "add", so the test seeds one entry through blyt_resource_table_test_push
 * (resource.h) — exercising the same realloc growth the cart/index loaders use. */
static void make_zstd_table(blyt_resource_table_t *t, uint32_t id) {
    blyt_resource_table_init(t);
    blyt_resource_entry_t *e = blyt_resource_table_test_push(t);
    assert(e != NULL);
    e->id = id;
    e->algo = BLYT_RES_ALGO_ZSTD;
    e->cdata = FRAME;
    e->clen = sizeof(FRAME);
    e->len = strlen(PAYLOAD);
    e->data = NULL; /* decoded lazily on first access */
    e->owned = NULL;
}

static void test_zstd_evict_then_rehydrate_identical(void) {
    blyt_resource_table_t t;
    make_zstd_table(&t, 1);
    blyt_resource_entry_t *e = blyt_resource_table_find_mut(&t, 1);
    assert(e);

    /* First access decodes into an owned buffer. */
    const uint8_t *first = blyt_resource_entry_data(e);
    assert(first != NULL);
    assert(e->owned != NULL);
    assert(e->len == strlen(PAYLOAD));
    assert(memcmp(first, PAYLOAD, e->len) == 0);

    /* Evictable (never loaded/pinned): eviction frees the owned bytes and
     * re-points the entry to its not-resident state. */
    assert(blyt_rl_is_evictable(&e->rl));
    size_t freed = blyt_resource_entry_evict(e);
    assert(freed == strlen(PAYLOAD));
    assert(e->owned == NULL);
    assert(e->data == NULL); /* not resident; next access rehydrates */

    /* Rehydration: re-decodes from the still-mapped frame, byte-identical. */
    const uint8_t *second = blyt_resource_entry_data(e);
    assert(second != NULL);
    assert(e->owned != NULL);
    assert(e->len == strlen(PAYLOAD));
    assert(memcmp(second, PAYLOAD, e->len) == 0);

    blyt_resource_table_clear(&t);
}

static void test_loaded_entry_is_not_evicted(void) {
    blyt_resource_table_t t;
    make_zstd_table(&t, 1);
    blyt_resource_entry_t *e = blyt_resource_table_find_mut(&t, 1);
    assert(blyt_resource_entry_data(e) != NULL);
    assert(e->owned != NULL);

    uint32_t h = blyt_rl_load(&e->rl, 1); /* residency held */
    assert(!blyt_rl_is_evictable(&e->rl));
    assert(blyt_resource_entry_evict(e) == 0); /* skipped: loaded */
    assert(e->owned != NULL); /* bytes intact */

    assert(blyt_rl_release(&e->rl, h) == 1);
    assert(blyt_resource_entry_evict(e) == strlen(PAYLOAD)); /* now evictable */
    blyt_resource_table_clear(&t);
}

static void test_pinned_entry_is_not_evicted(void) {
    blyt_resource_table_t t;
    make_zstd_table(&t, 1);
    blyt_resource_entry_t *e = blyt_resource_table_find_mut(&t, 1);
    assert(blyt_resource_entry_data(e) != NULL);

    blyt_rl_pin(&e->rl); /* within-frame pin held */
    assert(!blyt_rl_is_evictable(&e->rl));
    assert(blyt_resource_entry_evict(e) == 0); /* skipped: pinned */
    assert(e->owned != NULL);

    assert(blyt_rl_unpin(&e->rl) == 1);
    assert(blyt_resource_entry_evict(e) == strlen(PAYLOAD));
    blyt_resource_table_clear(&t);
}

static void test_evict_all_evictable_sweeps_only_eligible(void) {
    blyt_resource_table_t t;
    blyt_resource_table_init(&t);
    /* Three zstd entries; materialise all, then hold a load on #2 and a pin on
     * #3 so only #1 is evictable. */
    for (uint32_t id = 1; id <= 3; id++) {
        blyt_resource_entry_t *e = blyt_resource_table_test_push(&t);
        assert(e);
        e->id = id;
        e->algo = BLYT_RES_ALGO_ZSTD;
        e->cdata = FRAME;
        e->clen = sizeof(FRAME);
        e->len = strlen(PAYLOAD);
        e->data = NULL;
        e->owned = NULL;
        assert(blyt_resource_entry_data(e) != NULL);
    }
    uint32_t h2 = blyt_rl_load(&blyt_resource_table_find_mut(&t, 2)->rl, 2);
    blyt_rl_pin(&blyt_resource_table_find_mut(&t, 3)->rl);

    size_t freed = blyt_resource_table_evict_all_evictable(&t);
    assert(freed == strlen(PAYLOAD)); /* only #1 */
    assert(blyt_resource_table_find_mut(&t, 1)->owned == NULL);
    assert(blyt_resource_table_find_mut(&t, 2)->owned != NULL); /* loaded */
    assert(blyt_resource_table_find_mut(&t, 3)->owned != NULL); /* pinned */

    /* Drop the refs; a second sweep reclaims the rest. */
    (void)h2;
    blyt_rl_release(&blyt_resource_table_find_mut(&t, 2)->rl, h2);
    blyt_rl_unpin(&blyt_resource_table_find_mut(&t, 3)->rl);
    assert(blyt_resource_table_evict_all_evictable(&t) == 2 * strlen(PAYLOAD));
    blyt_resource_table_clear(&t);
}

static void test_reload_after_evict_mints_fresh_generation(void) {
    blyt_resource_table_t t;
    make_zstd_table(&t, 1);
    blyt_resource_entry_t *e = blyt_resource_table_find_mut(&t, 1);

    uint32_t h1 = blyt_rl_load(&e->rl, 1);
    assert(blyt_resource_entry_data(e) != NULL);
    assert(blyt_rl_release(&e->rl, h1) == 1); /* released: now evictable */
    assert(blyt_resource_entry_evict(e) == strlen(PAYLOAD));

    /* A fresh load after eviction mints a new generation; the pre-eviction
     * handle is stale and rejected (eviction never invalidates a *live* handle —
     * only an already-released one). */
    uint32_t h2 = blyt_rl_load(&e->rl, 1);
    assert(h2 != h1);
    assert(blyt_rl_release(&e->rl, h1) == 0); /* stale */
    assert(blyt_rl_release(&e->rl, h2) == 1);
    blyt_resource_table_clear(&t);
}

static void test_uncompressed_evict_is_noop(void) {
    /* A map-aliased uncompressed entry owns no bytes — evicting it reclaims
     * nothing and leaves its zero-copy alias intact. */
    blyt_resource_table_t t;
    blyt_resource_table_init(&t);
    blyt_resource_entry_t *e = blyt_resource_table_test_push(&t);
    assert(e);
    e->id = 1;
    e->algo = BLYT_RES_ALGO_NONE;
    e->data = (const uint8_t *)PAYLOAD; /* aliases the "map" */
    e->len = strlen(PAYLOAD);
    e->owned = NULL;
    assert(blyt_resource_entry_evict(e) == 0);
    assert(e->data == (const uint8_t *)PAYLOAD); /* alias untouched */
    blyt_resource_table_clear(&t);
}

int main(void) {
    test_zstd_evict_then_rehydrate_identical();
    test_loaded_entry_is_not_evicted();
    test_pinned_entry_is_not_evicted();
    test_evict_all_evictable_sweeps_only_eligible();
    test_reload_after_evict_mints_fresh_generation();
    test_uncompressed_evict_is_noop();
    printf("test_resource_eviction: all passed\n");
    return 0;
}
