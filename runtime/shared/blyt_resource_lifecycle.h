/*
 * blyt_resource_lifecycle.h — resource load/pin reference-count state machine
 * (ADR-0027, issue #123).
 *
 * Part of runtime/shared: freestanding portable C (stdint/stddef only, no libc,
 * no allocator, no stdio, no global ctors).  Compiled into BOTH the host runtime
 * (runtime/host/libblyt, backing the emulated/WASM/libretro ECALLs) and the
 * native guest libs (RV32, -nostdlib).  See blyt_fp_canon.h and issue #128.
 *
 * This is the determinism-critical core of the resource lifecycle: the
 * load_count / pin_count transitions and the load-generation rules that decide
 * whether a `load` handle is still valid.  The host and native paths deliver the
 * bytes differently (host copies into a per-frame guest scratch region and
 * returns a guest pointer; native returns a direct pointer into the retained
 * cart mmap) and own their own resource tables, but the *state machine* is this
 * one definition so the two can never drift (ADR-0007).
 *
 * Per-entry, not per-table: the caller owns the table and the id->entry lookup
 * and embeds one blyt_rl_state_t per resource; these functions are the pure
 * transitions over that state.  pin/unpin/load/release take the resource *id*
 * (load additionally mints, and release validates, a generation-stamped handle).
 *
 * Lifecycle model (ADR-0027 + the 2026-06 frame-scope amendment):
 *   - load(id) is residency/caching: idempotent, refcounted (load_count).  The
 *     returned handle packs the current load generation; a fresh load (the
 *     load_count 0->1 transition) bumps the generation, so a handle held across
 *     a full release becomes stale and release() rejects it.
 *   - pin(id)/unpin(id) is a within-frame raw-access window: refcounted
 *     (pin_count) and force-released at every frame boundary.  Independent of
 *     load_count — pin does not require a prior load.
 */

#ifndef BLYT_SHARED_RESOURCE_LIFECYCLE_H
#define BLYT_SHARED_RESOURCE_LIFECYCLE_H

#include <stdint.h>

#include "blyt_gen.h" /* blyt_gen_next — shared generation wrap (ADR-0096) */

/* Per-resource lifecycle counters.  Embed one in the caller's resource-table
 * entry; zero-initialised state (load_gen 0, both counts 0) means "present but
 * never loaded", which is correct for an eager-resident table at startup. */
typedef struct {
    uint32_t load_count; /* outstanding load() refs (residency) */
    uint32_t pin_count; /* outstanding pin() refs (within-frame raw access) */
    uint16_t load_gen; /* generation of the current load epoch; 0 = never loaded */
} blyt_rl_state_t;

/* ── load handle packing: (load_gen:16 | id:16) ──────────────────────────────
 * Mirrors the entity-ref convention (ADR-0096).  Handle 0 is the invalid
 * sentinel (BLYT_RESOURCE_INVALID).  The id occupies the low 16 bits, so the
 * load/release handle path supports ids 1..65535 (pin/unpin/text_get take the
 * full 32-bit id and are unaffected). */
static inline uint32_t blyt_rl_make_handle(uint32_t id, uint16_t gen) {
    return ((uint32_t)gen << 16) | (id & 0xFFFFu);
}
static inline uint16_t blyt_rl_handle_gen(uint32_t handle) {
    return (uint16_t)(handle >> 16);
}
static inline uint16_t blyt_rl_handle_id(uint32_t handle) {
    return (uint16_t)(handle & 0xFFFFu);
}

/* load(id): increment the residency refcount; on the 0->1 transition bump the
 * load generation (wrapping 65535 -> 1, never to 0) so a new load epoch mints a
 * fresh handle.  Idempotent while already loaded — returns the same handle.
 * Returns the generation-stamped handle for `id` (never 0 for a valid id). */
static inline uint32_t blyt_rl_load(blyt_rl_state_t *s, uint32_t id) {
    if (s->load_count == 0)
        s->load_gen = blyt_gen_next(s->load_gen); /* 0->1 on first ever load */
    s->load_count++;
    return blyt_rl_make_handle(id, s->load_gen);
}

/* release(handle): validate the handle against the current load epoch and
 * decrement the residency refcount.  Returns 1 on success, 0 if the handle is
 * stale (generation no longer current) or there is no outstanding load. */
static inline int blyt_rl_release(blyt_rl_state_t *s, uint32_t handle) {
    if (s->load_count == 0 || blyt_rl_handle_gen(handle) != s->load_gen)
        return 0;
    s->load_count--;
    return 1;
}

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

/* Eviction eligibility (ADR-0027 v2, #137): an entry's owned/decompressed bytes
 * may be reclaimed only when nothing references it — no outstanding load
 * residency and no within-frame pin.  This is the refcount half of the
 * predicate; persistence (ADR-0028, #160) is a separate per-entry property the
 * caller AND-checks.  Single-sourced here so the host eviction sweep and the
 * native bare-metal path can never disagree about what is evictable (ADR-0007).
 * Recency-ordered victim *selection* under partial pressure is advisory and
 * lives in the budget-aware caller (#158); this predicate is the hard gate. */
static inline int blyt_rl_is_evictable(const blyt_rl_state_t *s) {
    return s->load_count == 0 && s->pin_count == 0;
}

/* Frame-boundary force-release: any pin still held at the frame boundary is
 * dropped (ADR-0027 frame-scope amendment).  load_count is deliberately left
 * untouched — residency outlives the frame; only the raw-access window is
 * frame-scoped. */
static inline void blyt_rl_force_release_pins(blyt_rl_state_t *s) {
    s->pin_count = 0;
}

#endif /* BLYT_SHARED_RESOURCE_LIFECYCLE_H */
