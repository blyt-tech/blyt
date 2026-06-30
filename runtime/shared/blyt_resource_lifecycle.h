/*
 * blyt_resource_lifecycle.h — resource pin reference-count state machine
 * (ADR-0027 as amended by ADR-0134, issues #123 / #196).
 *
 * Part of runtime/shared: freestanding portable C (stdint/stddef only, no libc,
 * no allocator, no stdio, no global ctors).  Compiled into BOTH the host runtime
 * (runtime/host/libblyt, backing the emulated/WASM/libretro ECALLs) and the
 * native guest libs (RV32, -nostdlib).  See blyt_fp_canon.h and issue #128.
 *
 * The cart-held *residency handle* (load/release) is gone (ADR-0134, #196):
 * resources are referenced by their baked constant directly (see blyt_handle.h),
 * and the runtime owns residency — demand-load into an advisory cache, LRU
 * eviction under the 16 MB budget (#137/#158), declared-persistent pinning
 * (#160).  Nothing the cart holds can be invalidated.  What remains here is the
 * one determinism-critical piece the host and native paths must agree on: the
 * pin reference count and the eviction predicate.
 *
 * Per-entry, not per-table: the caller owns the table and the id->entry lookup
 * and embeds one blyt_rl_state_t per resource; these functions are the pure
 * transitions over that state.  pin(id)/unpin(id) take the (decoded) resource id.
 *
 * Lifecycle model (ADR-0027 frame-scope rule, ADR-0134 handle removal):
 *   - pin(id)/unpin(id) is a within-frame raw-access window: refcounted
 *     (pin_count) and force-released at every frame boundary.  It is the only
 *     cart-visible reference; residency otherwise lives entirely in the runtime.
 */

#ifndef BLYT_SHARED_RESOURCE_LIFECYCLE_H
#define BLYT_SHARED_RESOURCE_LIFECYCLE_H

#include <stdint.h>

/* Per-resource lifecycle counter.  Embed one in the caller's resource-table
 * entry; zero-initialised state (pin_count 0) means "present, not pinned", which
 * is correct for an eager-resident table at startup. */
typedef struct {
    uint32_t pin_count; /* outstanding pin() refs (within-frame raw access) */
} blyt_rl_state_t;

/* pin(id): take a within-frame raw-access reference. */
static inline void blyt_rl_pin(blyt_rl_state_t *s) {
    s->pin_count++;
}

/* unpin(id): drop a raw-access reference.  Returns 1 on success, 0 if there was
 * no outstanding pin (caller maps to an error). */
static inline int blyt_rl_unpin(blyt_rl_state_t *s) {
    if (s->pin_count == 0)
        return 0;
    s->pin_count--;
    return 1;
}

/* Eviction eligibility (ADR-0027 v2, #137; simplified by ADR-0134): an entry's
 * owned/decompressed bytes may be reclaimed only when nothing references it — no
 * within-frame pin.  This is the refcount half of the predicate; persistence
 * (ADR-0028, #160) is a separate per-entry property the caller AND-checks.
 * Single-sourced here so the host eviction sweep and the native bare-metal path
 * can never disagree about what is evictable (ADR-0007).  Recency-ordered victim
 * *selection* under partial pressure is advisory and lives in the budget-aware
 * caller (#158); this predicate is the hard gate. */
static inline int blyt_rl_is_evictable(const blyt_rl_state_t *s) {
    return s->pin_count == 0;
}

/* Frame-boundary force-release: any pin still held at the frame boundary is
 * dropped (ADR-0027 frame-scope amendment). */
static inline void blyt_rl_force_release_pins(blyt_rl_state_t *s) {
    s->pin_count = 0;
}

#endif /* BLYT_SHARED_RESOURCE_LIFECYCLE_H */
