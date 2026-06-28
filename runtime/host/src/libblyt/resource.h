#pragma once

/*
 * Host resource table (ADR-0040, ADR-0088, issue #91).
 *
 * Maps integer resource handles to their bytes. Two sources:
 *   - Packed (release): `.cart.resource.<id>` ELF sections in the cart; the
 *     entry data aliases the cart's mmap (not owned).
 *   - Dev (project-dir): `<dir>/resource-id-index` + content-addressed staging
 *     files under `<dir>/resources/`; each file is read into an owned buffer.
 *
 * The blyt_resource_text_get ECALL resolves a handle through this table and
 * copies the bytes into a guest scratch region (see cart_run.c).
 */

#include "blyt_runtime.h"

#include "blyt_resource_lifecycle.h" /* runtime/shared: load/pin refcount state */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t id;
    const uint8_t *data; /* decompressed bytes; aliases the cart map, points at
                          * `owned`, or NULL until a compressed entry is decoded */
    size_t len; /* decompressed length — authoritative even before a zstd entry
                 * is materialized (so headroom is known up front, #156) */
    void *owned; /* heap buffer to free on clear; NULL when data aliases the map */
    /* Per-resource compression for packed carts (#157, ADR-0026). For
     * BLYT_RES_ALGO_NONE, `data` aliases the map at load (zero-copy) and the
     * fields below are unused. For BLYT_RES_ALGO_ZSTD, `data` is NULL until
     * first access (decompressed lazily into `owned`); `cdata`/`clen` point at
     * the compressed body in the cart map. Dev-staging entries are always NONE. */
    uint8_t algo;
    const uint8_t *cdata;
    size_t clen;
    /* Dev-staging rehydration source (#137): for a project-dir entry, `owned` is
     * the *only* copy of the bytes (read from a content-addressed staging file),
     * so eviction would lose them unless they can be re-read. `src_path` is the
     * strdup'd staging path the entry was read from; eviction frees `owned` and
     * the next access re-reads this file. NULL for packed entries (which
     * rehydrate from `cdata`/the cart map instead). */
    char *src_path;
    blyt_rl_state_t rl; /* load/pin refcounts + load generation (#123) */
    /* Recency stamp for LRU victim selection under budget pressure (#158).
     * Bumped from the table's monotonic clock on each load/pin/access; advisory
     * (not cross-platform identical — only the non-evictable footprint carries
     * determinism), so the LRU order it imposes never affects alloc success. */
    uint32_t last_access;
} blyt_resource_entry_t;

typedef struct {
    blyt_resource_entry_t *entries;
    size_t count;
    size_t cap;
    uint32_t lru_clock; /* monotonic access counter; stamps entry->last_access (#158) */
} blyt_resource_table_t;

void blyt_resource_table_init(blyt_resource_table_t *t);
void blyt_resource_table_clear(blyt_resource_table_t *t);
const blyt_resource_entry_t *blyt_resource_table_find(const blyt_resource_table_t *t, uint32_t id);
/* Mutable lookup for the lifecycle ECALLs (pin/unpin/load/release mutate the
 * entry's refcounts). Returns NULL if `id` is absent. */
blyt_resource_entry_t *blyt_resource_table_find_mut(blyt_resource_table_t *t, uint32_t id);

/* Return the resource's decompressed bytes (length is `e->len`), decoding a
 * compressed (zstd) entry lazily on first access into `e->owned` and caching it
 * for reuse (#157, ADR-0026). An uncompressed entry returns its zero-copy map
 * alias unchanged. Returns NULL on decode failure / OOM. */
const uint8_t *blyt_resource_entry_data(blyt_resource_entry_t *e);
/* Frame-boundary force-release: drop every entry's pin count (ADR-0027). */
void blyt_resource_table_force_release_pins(blyt_resource_table_t *t);

/* Evict one entry's owned/decompressed bytes if it is eviction-eligible
 * (load_count==0 && pin_count==0, not persistent — ADR-0027 v2, #137). Frees
 * `e->owned` and re-points the entry to its not-resident state so the next
 * blyt_resource_entry_data() rehydrates byte-identically (zstd re-decode from
 * `cdata`, or dev-staging re-read from `src_path`). A no-op (returns 0) for an
 * uncompressed map-aliased entry (no owned bytes) or one that is not eligible.
 * Returns the number of owned bytes reclaimed. Never invalidates a live handle
 * — by construction it only touches entries with no load/pin reference. */
size_t blyt_resource_entry_evict(blyt_resource_entry_t *e);

/* Sweep the table, evicting every eligible entry (#137). The "evict all
 * evictable" forcing primitive behind the test hook, and the terminal fallback
 * of evict-before-fail (#158). Returns the total owned bytes reclaimed. */
size_t blyt_resource_table_evict_all_evictable(blyt_resource_table_t *t);

/* ── Unified-budget accounting + eviction policy (ADR-0008/0027 #158) ──────── */

/* The non-evictable footprint: Sum of decompressed length (e->len) over every
 * loaded/pinned (and, later, persistent) entry. This is the determinism-bearing
 * figure — the host publishes it to the guest so a `malloc`/`load` succeeds or
 * fails at the same logical point on every leg. Counts e->len up front, whether
 * or not the entry is currently materialized (a load reserves the budget). */
uint32_t blyt_resource_table_footprint(const blyt_resource_table_t *t);

/* Currently-resident *evictable* (reclaimable) cache bytes: Sum of e->len over
 * evictable entries that hold owned bytes. Advisory — bounded by eviction but
 * not part of any determinism contract. */
uint32_t blyt_resource_table_resident_evictable(const blyt_resource_table_t *t);

/* Currently-resident *decompressed* cache bytes — the `resource_cache_used`
 * figure for the introspection API (ADR-0029, #159): Sum of e->len over every
 * entry that holds an owned decompressed buffer (e->owned != NULL), whether or
 * not it is currently evictable. Zero-copy map-aliased (uncompressed) entries
 * have no owned bytes and never count — only *decompressed* resources occupy
 * cache. Advisory/history-dependent (residency follows LRU eviction), never a
 * determinism surface. */
uint32_t blyt_resource_table_resident_decompressed(const blyt_resource_table_t *t);

/* Stamp an entry as most-recently-used (advisory recency for LRU selection).
 * Call on each load/pin/access. */
void blyt_resource_table_touch(blyt_resource_table_t *t, blyt_resource_entry_t *e);

/* LRU-incremental evict-to-fit: evict evictable resident entries, least-recently
 * accessed first, until the resident evictable cache is <= max_resident_evictable
 * (or none remain). Frees the MINIMUM needed and stops the instant it fits
 * (#158, ADR-0027 — the pressure response). Returns the total bytes reclaimed. */
size_t blyt_resource_table_evict_to_fit(blyt_resource_table_t *t, uint32_t max_resident_evictable);

/* Append a zero-initialised entry and return it (grows the table). Exposed for
 * unit tests that construct entries directly (test_resource_eviction.c); the
 * production loaders use it internally. Returns NULL on OOM. */
blyt_resource_entry_t *blyt_resource_table_test_push(blyt_resource_table_t *t);

/* Release: populate from a packed cart's `.cart.resource.<id>` sections. Entry
 * data aliases the cart map. Clears existing entries first. Returns the number
 * of resources found. */
size_t blyt_resource_table_load_from_cart(blyt_resource_table_t *t, const blyt_cart_t *cart);

/* Dev: (re)populate from `<dir>/resource-id-index` and the content-addressed
 * staging files it references. Each file is read into an owned buffer. Clears
 * existing entries first. Returns 0 on success, -1 if the index is missing or
 * unreadable. Safe to call repeatedly (hot-swap re-reads the current index). */
int blyt_resource_table_load_from_index(blyt_resource_table_t *t, const char *dir);

/* Populate `t` the way a session's ctx.resources is populated: a packed cart's
 * embedded sections, else the staging directory beside the dev ELF (or
 * BLYT_RESOURCE_DIR). For standalone tables — the WASM pure-Lua host path lacks
 * a session to carry ctx.resources (#93/#120). Clears existing entries first. */
void blyt_resource_table_load_for_cart(blyt_resource_table_t *t, const blyt_cart_t *cart);

/* Accessor for the WASM host-Lua resource binding: a hybrid cart's resources
 * live in the session's run ctx (#93). */
blyt_resource_table_t *blyt_session_resources(blyt_session_t *s);
