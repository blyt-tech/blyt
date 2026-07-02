#pragma once

#include <stdint.h>

/*
 * Blyt ECALL numbers (a7 register before the ecall instruction, ADR-0085).
 *
 * Cart code never issues ecall instructions.  ECALLs are issued by
 * libblyt32.so's stub functions inside the rv32emu guest address space.
 * When rv32emu's on_ecall fires, the runtime dispatches on a7.
 *
 * Number space (ADR-0085):
 *   0        exit/abort — halt emulation; a0=0 clean, a0≠0 abort
 *   1–49     lifecycle (console_debug, frame_done, …)
 *   100–199  graphics
 *   …
 *
 * BLYT_TRACE convention: every ECALL case in blyt_ecall_handler() (and every
 * bridge sub-op in bridge_lua_op()) emits one typed blyt_tracef(BLYT_TRACE_API,
 * …) line using the cart-facing public name and the most useful formatting for
 * that API (ints as ints, slots as slots, strings dereferenced).  When adding
 * a new ECALL number here, add its trace line in the handler too — only the
 * default/unknown path falls back to a generic hex dump.
 */
#define BLYT_ECALL_EXIT 0 /* halt emulation; a0=exit code (0=clean, 1=abort) */
#define BLYT_ECALL_CONSOLE_DEBUG 1 /* blyt_console_debug: a0=ptr, a1=len */
#define BLYT_ECALL_FRAME_DONE 2 /* end of one update+draw cycle */
/* DAP hook: a0=source_vaddr, a1=source_len, a2=line, a3=depth
 * returns 1 (guest should pause) or 0 (continue); host sends "stopped" event */
#define BLYT_ECALL_DAP_HOOK 3
/* DAP send: a0=json_vaddr, a1=json_len; forwards JSON to DAP TCP client */
#define BLYT_ECALL_DAP_SEND 4
/* DAP recv: a0=buf_vaddr, a1=max_len; blocks until inspection command arrives;
 * writes JSON to guest buf; returns length in a0 (0 = continue/disconnect) */
#define BLYT_ECALL_DAP_RECV 5

/* ECALLs 6–8 are reserved for DAP extension operations (handled as literal
 * case labels in the ecall dispatch; see cart_run.c for details). */

/* Host-function return: fired by BLYT_TRAMPOLINE_FN_RETURN_ADDR when a
 * guest function invoked via blyt_session_begin_fn_call() returns.
 * The return value is already in a0 from the C function's ret instruction;
 * this ecall signals the host to stop driving rv32emu and read a0. */
#define BLYT_ECALL_HOST_FN_RETURN 9

/* Save/load ECALLs (ADR-0087, ADR-0125).
 * SAVE_WRITE: a0=slot; returns blyt_result_t in a0.
 * SAVE_READ:  a0=slot; returns blyt_result_t in a0; fires on_load_state. */
#define BLYT_ECALL_SAVE_WRITE 11
#define BLYT_ECALL_SAVE_READ 12

/* Lifecycle phase signal (issue #195 / #205; blyt_phase.h).  a0 = blyt_phase_t
 * (NONE/INIT/UPDATE/DRAW).  Emitted by the guest blyt_main around each cart
 * callback so the host can make all surface access draw()-only; the host stores
 * it on run-ctx->phase.  Cheap (two per frame) and carries no return value. */
#define BLYT_ECALL_PHASE 13

/* State buffer op (ADR-0009, ADR-0057, ADR-0058, ADR-0096).
 * a0 = sub-opcode (BUF_OP_*), remaining args per sub-opcode below.
 * For GET ops: a1=buf_h, a2=slot, a3=field_h; returns value bits in a0
 *   (f64: low 32 bits in a0, high 32 bits in a1).
 * For SET ops: a1=buf_h, a2=slot, a3=field_h, a4=value (uint32_t bit pattern;
 *   f64: a4=low 32 bits, a5=high 32 bits).
 * For ALLOC/FREE: a1=buf_h, a2=slot (FREE) or a2=out_slot ptr (ALLOC).
 * For REF: a1=buf_h, a2=slot; returns packed ref (gen:16|slot:16) or 0 in a0.
 * For REF_VALID: a1=buf_h, a2=ref; returns 1/0 in a0. */
#define BLYT_ECALL_BUF_OP 50

/* Resource pin API (ADR-0027, ADR-0040, ADR-0088, ADR-0134, issues #91/#123/#196).
 *
 * Resources are referenced by their baked console-wide constant (blyt_handle.h);
 * a0 carries that constant and the host decodes/classifies it (kind RESOURCE,
 * cart provenance) to the table id.  pin/unpin are the only resource ECALLs.
 *
 * Retired numbers (reserved, not renumbered):
 *   60 (RESOURCE_TEXT_GET): text access is a guest-side helper
 *      (blyt_resource_text_get in libblytcommon) built from pin -> copy -> unpin.
 *   63 (RESOURCE_LOAD) / 64 (RESOURCE_RELEASE): the cart-held residency handle is
 *      gone (ADR-0134, #196) — the runtime owns residency (demand-load + LRU
 *      evict #137 + persistent #160).
 *
 * RESOURCE_PIN:     a0=resource constant (in); a1=out_ptr vaddr, a2=out_size
 *   vaddr.  Copies the resource bytes into the per-frame guest scratch region,
 *   writes the guest pointer to *out_ptr and the byte length to *out_size,
 *   increments the pin count, and returns a blyt_result_t in a0.  The pointer is
 *   valid for the current frame only (force-released at the frame boundary,
 *   ADR-0027).
 * RESOURCE_UNPIN:   a0=resource constant; decrements the pin count; returns
 *   blyt_result_t. */
#define BLYT_ECALL_RESOURCE_PIN 61
#define BLYT_ECALL_RESOURCE_UNPIN 62
/* 63, 64 retired (RESOURCE_LOAD / RESOURCE_RELEASE) — see above. */

/* Memory introspection: loaded-resource enumeration (ADR-0029, issue #159,
 * epic #156 child 4).
 *
 * The SCALAR memory totals (cart_allocations, resource_cache_used, total_used,
 * budget_cap) are NOT an ECALL — they are published sums in the guest-readable
 * accounting block (blyt_mem_acct, #158) that blyt_mem_stats reads directly. The
 * only thing that needs the host is the variable-length list of currently-loaded
 * resources, which is host-owned table data the guest cannot reconstruct from a
 * fixed block — and it is resolved ON DEMAND here, never published per-mutation.
 *
 * MEM_RESOURCES: a0=out array vaddr (array of {u32 id, u32 size}) or 0; a1=cap
 *   (max pairs the array holds).  Writes up to `cap` {id,size} pairs for the
 *   currently-loaded resources and returns the *total* loaded count in a0 (may
 *   exceed `cap`; the array is truncated, the count is not).
 *
 * Determinism tiers (ADR-0029 amendment, #159): only budget_cap and the
 * success/failure outcome of an allocation are deterministic/safe to branch game
 * logic on.  resource_cache_used, total_used and the loaded-resource residency
 * are advisory (LRU/history-dependent) — a tuning signal, never game state. */
#define BLYT_ECALL_MEM_RESOURCES 70

/* Graphics / surface ECALLs (100–199, ADR-0052/0086/0008; issue #188 / Spike X,
 * generalized to runtime-managed surfaces in #195/#205).
 *
 * The paletted 2D drawing surface is Blyt32-specific; its tier-1 serviced ops
 * are host-side (reached by ECALL on the emulated path) and rasterize into a
 * *destination surface* via the shared integer rasterizer (runtime/shared/
 * blyt_raster.c).  Every op carries the destination surface handle in a0 (a
 * console-wide tagged u32, blyt_handle.h); BLYT_SCREEN (0x40000000) is the
 * built-in screen surface backed by session->pixels[].  A draw into the screen
 * sets cart_has_drawn, displacing the PM5544 test card; draws into off-screen
 * surfaces do not (they only reach the screen via a blit).  `gfx.*` is literal
 * sugar over BLYT_SCREEN.
 *
 * Coordinates are signed i32 passed in the argument registers; primitives clip
 * to the destination surface's bounds.  A handle that does not resolve to a live
 * surface (wrong kind, stale generation, out of phase) is a dev error / no-op.
 *   SURFACE_CLEAR:     a0=dst, a1=color (palette index); fills the whole surface.
 *   SURFACE_PIXEL:     a0=dst, a1=x, a2=y, a3=color.
 *   SURFACE_RECT_FILL: a0=dst, a1=x, a2=y, a3=w, a4=h, a5=color.
 *   SURFACE_LINE:      a0=dst, a1=x0, a2=y0, a3=x1, a4=y1, a5=color. */
#define BLYT_ECALL_SURFACE_CLEAR 100
#define BLYT_ECALL_SURFACE_PIXEL 101
#define BLYT_ECALL_SURFACE_RECT_FILL 102
#define BLYT_ECALL_SURFACE_LINE 103

/* Raw-framebuffer acquire/present (issue #188 / Spike X, Q1).  The fixed-region
 * mechanism from the spike; superseded by the tier-2 surface acquire/release
 * lock (SURFACE_ACQUIRE / SURFACE_RELEASE below) in #205 but kept here until
 * that lands.
 *   GFX_ACQUIRE: no args; returns the guest VA of that region in a0.  The cart
 *     writes palette indices directly into it (no per-pixel ECALL).
 *   GFX_PRESENT: no args; copies the whole region into session->pixels[] and
 *     sets cart_has_drawn (displacing the test card). */
#define BLYT_ECALL_GFX_ACQUIRE 104
#define BLYT_ECALL_GFX_PRESENT 105

/* Surface lifecycle + blit (tier-1, #205).
 *   SURFACE_CREATE:  a0=w, a1=h; returns a surface handle in a0 (BLYT_HANDLE_NONE
 *     on failure — invalid size or over the 16 MB budget).  Blank (index 0)
 *     content; draw-scoped (auto-reaped at end of draw()); counts against the
 *     unified memory budget (#158).
 *   SURFACE_DESTROY: a0=surface; optional early free (a no-op on BLYT_SCREEN or a
 *     stale/invalid handle).
 *   SURFACE_BLIT:    a0=dst, a1=src, a2=x, a3=y; copies the whole src surface into
 *     dst at (x,y), clipped to dst; index-copy (palette is global). */
#define BLYT_ECALL_SURFACE_CREATE 106
#define BLYT_ECALL_SURFACE_DESTROY 107
#define BLYT_ECALL_SURFACE_BLIT 108

/* Tier-2 per-pixel lock (acquire/release, #205).  A cart that needs raw per-pixel
 * access acquires a surface: the runtime materializes its canonical buffer into a
 * guest-addressable region and hands back a blyt_lock_t {pixels, stride, w, h,
 * token}.  The cart reads/writes pixels directly and/or draws with the
 * freestanding blyt_raster_* primitives (guest-side, no ECALL), then releases —
 * flushing the region back to the canonical buffer and invalidating the token.
 * Exclusive per surface; distinct surfaces may be locked at once.
 *   SURFACE_ACQUIRE: a0=surface, a1=out_lock vaddr (blyt_lock_t to fill).  On
 *     failure (unresolvable surface, already locked, no region) the lock's token
 *     is BLYT_HANDLE_NONE and pixels is 0.
 *   SURFACE_RELEASE: a0=lock-view token; flushes + invalidates.  A no-op on a
 *     stale/foreign token (kind or generation mismatch). */
#define BLYT_ECALL_SURFACE_ACQUIRE 109
#define BLYT_ECALL_SURFACE_RELEASE 110

/* Palette load (issue #201, ADR-0042/0086).  Loads one of the four
 * runtime-bundled built-in palettes wholesale into the global 256-entry
 * screen palette.
 *   GFX_PALETTE_SET: a0=palette handle (console-wide tagged u32, runtime
 *     provenance).  A no-op on a handle that does not resolve to a built-in
 *     palette (unknown id, wrong kind/provenance). */
#define BLYT_ECALL_GFX_PALETTE_SET 111
/* Sub-opcodes for BLYT_ECALL_BUF_OP (a0).
 * type_tag encoding: 0=i8 1=u8 2=i16 3=u16 4=i32 5=u32 6=f32 7=bool 8=f64 */
#define BUF_OP_GET_F32 1
#define BUF_OP_SET_F32 2
#define BUF_OP_GET_I32 3
#define BUF_OP_SET_I32 4
#define BUF_OP_GET_U32 5
#define BUF_OP_SET_U32 6
#define BUF_OP_GET_I16 7
#define BUF_OP_SET_I16 8
#define BUF_OP_GET_U16 9
#define BUF_OP_SET_U16 10
#define BUF_OP_GET_I8 11
#define BUF_OP_SET_I8 12
#define BUF_OP_GET_U8 13
#define BUF_OP_SET_U8 14
#define BUF_OP_GET_BOOL 15
#define BUF_OP_SET_BOOL 16
#define BUF_OP_ALLOC_SLOT 17
#define BUF_OP_FREE_SLOT 18
#define BUF_OP_REF 19
#define BUF_OP_REF_VALID 20
#define BUF_OP_GET_F64 21 /* Spike U: a0=lo, a1=hi */
#define BUF_OP_SET_F64 22 /* Spike U: a4=lo, a5=hi */

/* Lua C API bridge op (ADR-0130, WASM hybrid carts only).
 * Issued by the bridge-stub variant of libblyt32lua.so while a bridged
 * Lua→native call is in flight.  a0=opcode (BLYT_LUA_OP_*), a1=call token,
 * a2–a5=op args.  Returns a0=status (BLYT_LUA_ST_*), a1=value, a2=aux.
 * Outside the bridged-call window, or with a bad token/opcode, this traps. */
#define BLYT_ECALL_LUA_OP 10

/* Bridge opcodes (a0).  One per bridged Lua C API operation; the dispatch
 * switch in lua_bridge.c is the ADR-0118 enforcement point on WASM — the
 * loading/compiling class has no opcodes by construction. */
enum {
    BLYT_LUA_OP_GETTOP = 1, /* () -> top */
    BLYT_LUA_OP_SETTOP = 2, /* (idx) */
    BLYT_LUA_OP_PUSHVALUE = 3, /* (idx) */
    BLYT_LUA_OP_TYPE = 4, /* (idx) -> LUA_T* */
    BLYT_LUA_OP_PUSHNIL = 5, /* () */
    BLYT_LUA_OP_PUSHBOOLEAN = 6, /* (b) */
    BLYT_LUA_OP_PUSHINTEGER = 7, /* (n) */
    BLYT_LUA_OP_PUSHNUMBER = 8, /* (lo_u32, hi_u32) — f64 lo+hi pair (Spike U) */
    BLYT_LUA_OP_PUSHLSTRING = 9, /* (ptr, len) */
    BLYT_LUA_OP_TOINTEGERX = 10, /* (idx) -> n, aux=isnum */
    BLYT_LUA_OP_TONUMBERX = 11, /* (idx) -> lo_u32+hi_u32 f64; ST_NIL if !isnum (Spike U) */
    BLYT_LUA_OP_TOBOOLEAN = 12, /* (idx) -> 0/1 */
    BLYT_LUA_OP_TOLSTRING = 13, /* (idx, buf, cap) -> wrote, aux=full len */
    BLYT_LUA_OP_CREATETABLE = 14, /* (narr, nrec) */
    BLYT_LUA_OP_GETFIELD = 15, /* (idx, k_ptr, k_len) -> type */
    BLYT_LUA_OP_SETFIELD = 16, /* (idx, k_ptr, k_len) */
    BLYT_LUA_OP_GETI = 17, /* (idx, i) -> type */
    BLYT_LUA_OP_SETI = 18, /* (idx, i) */
    BLYT_LUA_OP_RAWLEN = 19, /* (idx) -> len */
    BLYT_LUA_OP_NEXT = 20, /* (idx) -> 0/1 */
    BLYT_LUA_OP_GETGLOBAL = 21, /* (name_ptr, name_len) -> type */
    BLYT_LUA_OP_SETGLOBAL = 22, /* (name_ptr, name_len) */
    BLYT_LUA_OP_ERROR = 23, /* () — never returns to the guest */
    BLYT_LUA_OP_ERRMSG = 24, /* (msg_ptr, msg_len) — never returns */
};

/* Bridge op status (returned in a0).  Lua errors never return: the host
 * halts emulation and raises from the trampoline continuation instead. */
#define BLYT_LUA_ST_OK 0
#define BLYT_LUA_ST_RETRY 1 /* TOLSTRING: cap too small; aux has needed len */
#define BLYT_LUA_ST_NIL 2 /* TOLSTRING: value not string/number (NULL return) */

/*
 * EXIT trampoline — injected into rv32emu guest memory by the runtime.
 *
 * When blyt_main returns, the CPU jumps to BLYT_TRAMPOLINE_EXIT_ADDR
 * (placed in RA before calling blyt_main).  The trampoline sets a0=0
 * (clean exit) and a7=0 (BLYT_ECALL_EXIT), then issues ecall so the
 * host can halt the emulator and report BLYT_RUN_OK.
 *
 * abort() in libblytc.so uses the same ecall but with a0=1, which the
 * handler maps to BLYT_RUN_ERR_ABORT so frontends can distinguish a
 * normal return from a fatal internal error.
 *
 * Layout at BLYT_TRAMPOLINE_BASE (16 bytes):
 *   addi x10, x0, 0   ; li a0, 0  (exit code: 0 = clean)
 *   addi x17, x0, 0   ; li a7, BLYT_ECALL_EXIT
 *   ecall
 *   unimp              ; 0x00000000 — should never be reached
 *
 * Address chosen to be above the 16 MiB cart RAM limit and below the
 * 64 MiB arena and 128 MiB library base.
 */
#define BLYT_TRAMPOLINE_BASE 0x01000000u
#define BLYT_TRAMPOLINE_EXIT_ADDR BLYT_TRAMPOLINE_BASE
/* FN_RETURN stub at +64 from exit (16 bytes each, 48 bytes gap for growth). */
#define BLYT_TRAMPOLINE_FN_RETURN_ADDR (BLYT_TRAMPOLINE_EXIT_ADDR + 64u)

#define RV32_LI_A0_0 UINT32_C(0x00000513) /* addi x10, x0, 0 */
#define RV32_LI_A7_0 UINT32_C(0x00000893) /* addi x17, x0, 0 */
#define RV32_LI_A7_9 UINT32_C(0x00900893) /* addi x17, x0, 9 (BLYT_ECALL_HOST_FN_RETURN) */
#define RV32_ECALL UINT32_C(0x00000073)
#define RV32_UNIMP UINT32_C(0x00000000)
