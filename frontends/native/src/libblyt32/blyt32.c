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

/* ── Tier-1 surface ops (ADR-0052/0086/0008, #188 / #195 / #205) ─────────────
 *
 * Native entry points the cart resolves directly (no ECALL); each forwards to
 * the shared integer rasterizer over the destination surface's buffer.  The
 * gfx.* screen shorthand is inline sugar over these (blyt.h).  Off-screen
 * surfaces (blyt_surface_create/destroy/blit) land here alongside the QEMU gate
 * coverage; for now the screen (BLYT_SCREEN, slot 0) is the only destination
 * and any other handle is a defined no-op. */

static uint8_t *native_resolve_screen(blyt_surface_h dst) {
    /* Draw()-only enforcement (#205): surface access outside draw() is a defined
     * no-op on the release path (this bare-metal build) — mirrors the host
     * gate's release semantics so the QEMU gate matches the emulated legs.  A
     * NULL buffer makes every screen op fall through to nothing. */
    if (blyt_phase_current() != BLYT_PHASE_DRAW)
        return (uint8_t *)0;
    return dst == BLYT_SCREEN ? s_framebuffer : (uint8_t *)0;
}

void blyt_surface_clear(blyt_surface_h dst, uint8_t color) {
    uint8_t *fb = native_resolve_screen(dst);
    if (fb)
        blyt_raster_clear(fb, NATIVE_FB_W, NATIVE_FB_W, NATIVE_FB_H, color);
}

void blyt_surface_pixel(blyt_surface_h dst, int32_t x, int32_t y, uint8_t color) {
    uint8_t *fb = native_resolve_screen(dst);
    if (fb)
        blyt_raster_pixel(fb, NATIVE_FB_W, NATIVE_FB_W, NATIVE_FB_H, x, y, color);
}

void blyt_surface_rect_fill(blyt_surface_h dst, int32_t x, int32_t y, int32_t w, int32_t h,
                            uint8_t color) {
    uint8_t *fb = native_resolve_screen(dst);
    if (fb)
        blyt_raster_rect_fill(fb, NATIVE_FB_W, NATIVE_FB_W, NATIVE_FB_H, x, y, w, h, color);
}

void blyt_surface_line(blyt_surface_h dst, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                       uint8_t color) {
    uint8_t *fb = native_resolve_screen(dst);
    if (fb)
        blyt_raster_line(fb, NATIVE_FB_W, NATIVE_FB_W, NATIVE_FB_H, x0, y0, x1, y1, color);
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
