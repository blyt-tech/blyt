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
    blyt_rl_state_t rl; /* load/pin refcounts + load generation (#123) */
} blyt_resource_entry_t;

typedef struct {
    blyt_resource_entry_t *entries;
    size_t count;
    size_t cap;
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
