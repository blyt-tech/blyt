/*
 * libblyt32 — Blyt32 variant shared library, native RISC-V path.
 *
 * The Blyt32 variant carries the variant-specific graphics surface (ADR-0086):
 * the 320x240 256-colour paletted framebuffer and its drawing primitives.  On
 * the emulated path these are ECALL stubs the host services; here the guest
 * library *is* the runtime, so the primitives rasterize directly into an
 * in-process back buffer using the SAME shared integer rasterizer
 * (runtime/shared/blyt_raster.c) the host and wasm compiles use — the Q2 proof
 * that one source produces bit-identical pixels on every target (issue #188 /
 * Spike X).
 *
 * The variant-agnostic real-work impls (state buffers, save/load, frame-boundary
 * FP determinism, console debug, startup, exit) live in the native
 * libblytcommon variant (frontends/native/src/libblytcommon/).  blyt_exit stays
 * there too.  This library declares DT_NEEDED libblytcommon.so + libblytc.so so
 * the cart resolves the relocated symbols and the system C library over the
 * chain to ld-blyt.so.1.
 */

#include <stddef.h>
#include <stdint.h>

#include "blyt.h"
#include "blyt_frame_hash.h" /* runtime/shared: FNV-1a 64 over the paletted fb */
#include "blyt_handle.h" /* runtime/shared: SURFACE/LOCKVIEW encode/classify (#196/#205) */
#include "blyt_mem_budget.h" /* runtime/shared: unified 16 MB budget (#158) */
#include "blyt_native_trace.h" /* getenv + raw-write helpers (no libc stdio) */
#include "blyt_phase.h" /* runtime/shared: draw()-only phase (#205) */
#include "blyt_raster.h" /* runtime/shared: integer rasterizer core */

/* The paletted surface geometry.  Matches the host's BLYT_FRAME_W/H
 * (runtime/host/include/blyt_runtime.h); the proper graphics API will
 * centralise these in a shared header — for the spike they are defined at each
 * boundary that needs them and the cross-leg hash test pins that they agree. */
#define NATIVE_FB_W 320
#define NATIVE_FB_H 240

/* The runtime-owned back buffer (one byte per pixel, the palette index).
 * BSS / zero-initialised: a cart that never draws presents a black frame, and
 * every probe writes all pixels it cares about before reading the hash. */
static uint8_t s_framebuffer[NATIVE_FB_W * NATIVE_FB_H];

/* ── Surface registry (ADR-0052/0086/0008, #188 / #195 / #205) ───────────────
 *
 * On bare metal the guest library *is* the runtime, so surfaces are a
 * process-local pool (no ECALL, no host registry).  Slot 0 is BLYT_SCREEN,
 * aliasing s_framebuffer; slots 1.. are off-screen surfaces carved from a
 * static bump arena.  The pool mirrors the host registry in cart_run.c but is
 * simpler: a tier-2 lock hands back the canonical buffer pointer *directly*
 * (same address space — no copy-in/out), so in-lock drawing writes the surface
 * in place.  Surfaces are draw-scoped: reaped at each frame boundary
 * (blyt_gfx_reap_surfaces, called from the native blyt_frame_done).
 *
 * The arena is sized to the unified 16 MB budget so a create that would overflow
 * it returns BLYT_HANDLE_NONE — the bare-metal analogue of the host's budget
 * rejection (the emulated legs assert the exact number; here parity is the
 * NONE-on-overflow behaviour, not shared heap accounting).  BSS zero-fill gives
 * an empty pool with no constructor (none run on this path, #43); the pool is
 * lazily initialised on first use. */

#define NATIVE_SURFACE_MAX 64 /* slots incl. slot 0 = screen */
#define NATIVE_SURFACE_MAX_DIM 8192 /* overflow guard; the budget is the real cap */

typedef struct {
    uint8_t *pixels; /* slot 0 -> s_framebuffer; off-screen -> into the arena */
    int32_t w, h;
    uint16_t gen; /* bumped on reap/destroy — a stale surface handle goes stale */
    uint8_t in_use;
    uint8_t is_screen;
    uint8_t locked;
    uint16_t lock_gen; /* bumped on release — a released lock token goes stale */
} native_surface_t;

static native_surface_t s_surfaces[NATIVE_SURFACE_MAX];
static uint8_t s_surface_arena[BLYT_MEM_BUDGET_BYTES]; /* off-screen bump arena */
static uint32_t s_surface_bump;
static uint8_t s_surfaces_init;

static void native_surfaces_init(void) {
    if (s_surfaces_init)
        return;
    s_surfaces[0].pixels = s_framebuffer;
    s_surfaces[0].w = NATIVE_FB_W;
    s_surfaces[0].h = NATIVE_FB_H;
    s_surfaces[0].in_use = 1;
    s_surfaces[0].is_screen = 1;
    s_surfaces_init = 1;
}

/* Resolve a surface handle to its slot, enforcing draw()-only access (#205):
 * outside draw() this returns NULL, so every access op no-ops on the release
 * bare-metal build — matching the host gate's release semantics.  A wrong kind
 * or stale generation also returns NULL. */
static native_surface_t *native_resolve_surface(blyt_surface_h h) {
    native_surfaces_init();
    if (blyt_phase_current() != BLYT_PHASE_DRAW)
        return (native_surface_t *)0;
    if (!blyt_handle_is_surface(h))
        return (native_surface_t *)0;
    uint32_t idx = blyt_dyn_decode_index(h);
    if (idx >= NATIVE_SURFACE_MAX)
        return (native_surface_t *)0;
    native_surface_t *s = &s_surfaces[idx];
    if (!s->in_use || (uint16_t)blyt_dyn_decode_gen(h) != s->gen)
        return (native_surface_t *)0;
    return s;
}

/* Reject handle-path access to a *locked* surface (#207): while a tier-2 lock is
 * held the lock owns the surface, so every tier-1 op / blit / destroy reached via
 * the surface handle is a no-op on bare metal — the release-build rejection that
 * mirrors the host gate (cart_run.c) and kills the emulated-vs-native divergence.
 * (There is no debug build on metal; the reject is always the silent no-op.)
 * acquire uses the base resolver so it can still reject a *re*-acquire through its
 * own locked check. */
static native_surface_t *native_resolve_drawable(blyt_surface_h h) {
    native_surface_t *s = native_resolve_surface(h);
    return (s && s->locked) ? (native_surface_t *)0 : s;
}

blyt_surface_h blyt_surface_create(int32_t w, int32_t h) {
    native_surfaces_init();
    if (blyt_phase_current() != BLYT_PHASE_DRAW)
        return BLYT_HANDLE_NONE;
    if (w <= 0 || h <= 0 || w > NATIVE_SURFACE_MAX_DIM || h > NATIVE_SURFACE_MAX_DIM)
        return BLYT_HANDLE_NONE;
    uint32_t bytes = (uint32_t)w * (uint32_t)h;
    if ((uint64_t)s_surface_bump + bytes > sizeof(s_surface_arena))
        return BLYT_HANDLE_NONE; /* over budget */
    uint32_t idx = 0;
    for (uint32_t i = 1; i < NATIVE_SURFACE_MAX; i++) {
        if (!s_surfaces[i].in_use) {
            idx = i;
            break;
        }
    }
    if (idx == 0) /* registry full */
        return BLYT_HANDLE_NONE;
    uint8_t *buf = &s_surface_arena[s_surface_bump];
    s_surface_bump += bytes;
    blyt_raster_clear(buf, w, w, h, 0); /* blank = palette index 0 */
    native_surface_t *s = &s_surfaces[idx];
    s->pixels = buf;
    s->w = w;
    s->h = h;
    s->in_use = 1;
    s->is_screen = 0;
    return blyt_surface_encode(s->gen, idx);
}

void blyt_surface_destroy(blyt_surface_h surface) {
    native_surface_t *s = native_resolve_drawable(surface); /* locked -> reject (#207) */
    if (s && !s->is_screen) {
        s->in_use = 0;
        s->gen++; /* invalidate outstanding handles */
    }
}

void blyt_surface_clear(blyt_surface_h dst, uint8_t color) {
    native_surface_t *s = native_resolve_drawable(dst); /* locked -> reject (#207) */
    if (s)
        blyt_raster_clear(s->pixels, s->w, s->w, s->h, color);
}

void blyt_surface_pixel(blyt_surface_h dst, int32_t x, int32_t y, uint8_t color) {
    native_surface_t *s = native_resolve_drawable(dst); /* locked -> reject (#207) */
    if (s)
        blyt_raster_pixel(s->pixels, s->w, s->w, s->h, x, y, color);
}

void blyt_surface_rect_fill(blyt_surface_h dst, int32_t x, int32_t y, int32_t w, int32_t h,
                            uint8_t color) {
    native_surface_t *s = native_resolve_drawable(dst); /* locked -> reject (#207) */
    if (s)
        blyt_raster_rect_fill(s->pixels, s->w, s->w, s->h, x, y, w, h, color);
}

void blyt_surface_line(blyt_surface_h dst, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                       uint8_t color) {
    native_surface_t *s = native_resolve_drawable(dst); /* locked -> reject (#207) */
    if (s)
        blyt_raster_line(s->pixels, s->w, s->w, s->h, x0, y0, x1, y1, color);
}

void blyt_surface_blit(blyt_surface_h dst, blyt_surface_h src, int32_t x, int32_t y) {
    /* Reject if either endpoint is locked — a blit as dst OR src (#207). */
    native_surface_t *d = native_resolve_drawable(dst);
    native_surface_t *s = native_resolve_drawable(src);
    if (d && s)
        blyt_raster_blit(d->pixels, d->w, d->w, d->h, s->pixels, s->w, s->w, s->h, x, y);
}

int32_t blyt_surface_acquire(blyt_surface_h surface, blyt_lock_t *out) {
    native_surface_t *s = native_resolve_surface(surface);
    if (!s || s->locked) {
        if (out) {
            out->pixels = (uint8_t *)0;
            out->stride = 0;
            out->w = 0;
            out->h = 0;
            out->token = BLYT_HANDLE_NONE;
        }
        return 0;
    }
    s->locked = 1;
    /* Direct canonical pointer: bare metal shares one address space, so in-lock
     * drawing writes the surface buffer in place (no materialization copy). */
    out->pixels = s->pixels;
    out->stride = s->w;
    out->w = s->w;
    out->h = s->h;
    out->token = blyt_lockview_encode(s->lock_gen, blyt_dyn_decode_index(surface));
    return 1;
}

void blyt_surface_release(blyt_lock_t *lock) {
    if (!lock)
        return;
    uint32_t token = lock->token;
    if (!blyt_handle_is_lockview(token))
        return;
    uint32_t idx = blyt_dyn_decode_index(token);
    if (idx >= NATIVE_SURFACE_MAX)
        return;
    native_surface_t *s = &s_surfaces[idx];
    if (!s->locked || (uint16_t)blyt_dyn_decode_gen(token) != s->lock_gen)
        return; /* stale or foreign token */
    s->locked = 0;
    s->lock_gen++; /* the token is now stale */
    /* Nothing to flush: the lock exposed the canonical buffer directly. */
}

/* Reap off-screen surfaces at the frame boundary (draw-scoped, #205): free the
 * arena, drop every off-screen slot, and force-release any lock the cart left
 * held.  Called from the native blyt_frame_done via a weak reference (like
 * blyt_gfx_on_frame_boundary), so libblytcommon stays graphics-agnostic. */
void blyt_gfx_reap_surfaces(void) {
    native_surfaces_init();
    for (uint32_t i = 1; i < NATIVE_SURFACE_MAX; i++) {
        native_surface_t *s = &s_surfaces[i];
        if (s->locked) {
            s->locked = 0;
            s->lock_gen++;
        }
        if (s->in_use) {
            s->in_use = 0;
            s->gen++;
        }
    }
    if (s_surfaces[0].locked) { /* screen: force-release, never freed */
        s_surfaces[0].locked = 0;
        s_surfaces[0].lock_gen++;
    }
    s_surface_bump = 0;
}

/* ── Direct framebuffer access (issue #188 / Spike X, Q1) ────────────────────
 *
 * acquire hands the cart a pointer to the live back buffer so it can write
 * palette indices directly (no per-pixel ECALL); present is the flush point.
 * On the emulated path present copies the guest region into the host
 * framebuffer; here the cart already wrote the live buffer, so present is a
 * no-op kept for contract symmetry across execution models. */

uint8_t *blyt_gfx_acquire(void) {
    return s_framebuffer;
}

void blyt_gfx_present(void) {
    /* Nothing to flush: the cart wrote s_framebuffer in place. */
}

/* ── Frame-boundary hash emit (issue #188 / Spike X, Q2 capture) ─────────────
 *
 * The cross-leg determinism harness emits one "[blyt:fbhash] <hex>" line per
 * frame when BLYT_FRAME_HASH is set, so the integration suite can assert the
 * native bare-metal framebuffer hashes identically to the host/wasm/libretro
 * legs straight from captured stdout.  The host runtime emits this from its
 * frame_done hook; on bare metal frame_done lives in the variant-agnostic
 * libblytcommon, which calls this strong definition through a weak reference
 * (the same cross-lib pattern the Lua stubs use for blyt_console_debug) so it
 * stays graphics-agnostic.  Off by default; one cached getenv when unset. */
void blyt_gfx_on_frame_boundary(void) {
    static int s_on = -1;
    if (s_on < 0)
        s_on = getenv("BLYT_FRAME_HASH") != 0 ? 1 : 0;
    if (!s_on)
        return;

    uint64_t h = blyt_frame_hash(s_framebuffer, (size_t)NATIVE_FB_W * (size_t)NATIVE_FB_H);

    /* Hand-format "[blyt:fbhash] %016llx\n" (no snprintf in these libs) and
     * write to stdout (fd 1) — the channel run_cart_native asserts on, matching
     * the host emit. */
    char buf[32];
    const char pfx[] = "[blyt:fbhash] ";
    unsigned int i = 0;
    for (unsigned int j = 0; j < sizeof(pfx) - 1; j++)
        buf[i++] = pfx[j];
    for (int shift = 60; shift >= 0; shift -= 4)
        buf[i++] = "0123456789abcdef"[(h >> shift) & 0xFu];
    buf[i++] = '\n';

    register long a0 __asm__("a0") = 1; /* STDOUT_FILENO */
    register const char *a1 __asm__("a1") = buf;
    register long a2 __asm__("a2") = (long)i;
    register long a7 __asm__("a7") = 64; /* SYS_write */
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
}
