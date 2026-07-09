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

/* Sentinel meaning "no rv32 size was published for this allocation" — the fork
 * resets to it after every published request, and the runner restores it after
 * consuming one. An allocation the runner sees while the pending size is UNSET
 * did NOT come through the fork's luaM typed layer: it is a *raw* byte buffer
 * requested straight through the lua_Alloc callback (the auxlib string buffer's
 * heap box and the external-string body it becomes — lauxlib.c resizebox). A raw
 * byte buffer holds no pointers, so its size is pointer-width-independent and the
 * runner accounts it at the host `nsize` verbatim (which already equals the rv32
 * size). No real allocation is SIZE_MAX bytes under the 16 MB budget, so the
 * sentinel can never collide with a genuine published size. */
#define BLYT_HOSTLUA_HEAP_RV_UNSET ((size_t)-1)

/* rv32-equivalent size of the NEXT physical allocation the fork will request via
 * the lua_Alloc callback (set by the fork's luaM layer under the seam; read and
 * consumed by the runner's allocator). Only meaningful for malloc/realloc
 * requests (nsize > 0); frees recover the shadow block from the runner's own
 * per-block prefix, so this is not consulted on free. Holds
 * BLYT_HOSTLUA_HEAP_RV_UNSET between a runner consume and the next fork publish —
 * see that constant for how raw (non-luaM) allocations are accounted. */
extern size_t blyt_hostlua_heap_rv_pending;

/* Set to 1 by the fork's luaM layer immediately before a Lua thread's data stack
 * or CallInfo is (re)allocated — VM execution scratch, not cart data (#231). The
 * runner's allocator reads it (and resets it) to route that block through the
 * arena's no-acct path, so guest_heap_used and the 16 MB fail-point are
 * independent of how the runtime drives the cart: the native runner calls
 * init()/update()/draw() directly from C, the wasm runner resumes them inside a
 * driver coroutine (one extra call frame + stack slots), and without this the
 * coroutine frame would count as cart-attributable on wasm only. Consumed
 * back-to-back with the allocation (single-threaded VM), exactly like
 * blyt_hostlua_heap_rv_pending; the implementation is identical on every host-Lua
 * leg, so any residual (e.g. a near-cap emergency-GC retry) is byte-identical
 * across legs and the cross-leg contract holds. Defined by the fork whenever the
 * host-Lua accounting layer is engaged (BLYT_HOSTLUA_HEAP_ACCT). */
extern int blyt_hostlua_heap_stack_pending;

#endif /* BLYT_SHARED_HOSTLUA_HEAP_H */
