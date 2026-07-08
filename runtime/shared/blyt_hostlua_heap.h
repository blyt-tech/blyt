/*
 * blyt_hostlua_heap.h — the rv32 heap-accounting seam's fork↔runner handoff
 * (BLYT_HOSTLUA_HEAP_SEAM, #231, epic #230).
 *
 * The native host-Lua fast path runs the Lua VM at 64-bit host object sizes, but
 * `guest_heap_used` must be counted at the 32-bit canonical (DIRECTION 1) so it
 * is byte-identical to the wasm32 host-Lua leg (the determinism contract,
 * ADR-0029). The Lua fork (compiled with BLYT_HOSTLUA_HEAP_SEAM) knows, at each
 * allocation, the rv32-equivalent size (from the generated rv32 sizeof table);
 * it publishes that here immediately before invoking the lua_Alloc callback. The
 * runner's allocator (runtime/host/.../cart_run_hostlua.c) reads it to drive a
 * separate rv32-sized *shadow* arena that produces the canonical
 * `guest_heap_used` and the 16 MB fail-point, while the physical bytes come from
 * plain host malloc (unbounded — a 16 MB-budget cart may use ~2× host RAM).
 *
 * Single-threaded VM (one OS thread drives the Lua state), so a plain global is
 * safe: it is written and read back-to-back with no allocation in between. The
 * symbol is defined unconditionally by the fork (lmem.c) so the runner links
 * against it whether or not a given VM build engages the seam.
 */

#ifndef BLYT_SHARED_HOSTLUA_HEAP_H
#define BLYT_SHARED_HOSTLUA_HEAP_H

#include <stddef.h>

/* rv32-equivalent size of the NEXT physical allocation the fork will request via
 * the lua_Alloc callback (set by the fork's luaM layer under the seam; read and
 * consumed by the runner's allocator). Only meaningful for malloc/realloc
 * requests (nsize > 0); frees recover the shadow block from the runner's own
 * per-block prefix, so this is not consulted on free. */
extern size_t blyt_hostlua_heap_rv_pending;

#endif /* BLYT_SHARED_HOSTLUA_HEAP_H */
