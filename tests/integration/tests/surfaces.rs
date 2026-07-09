//! Runtime-managed surfaces — the #195 PR-B surface model (issue #205).
//!
//! Builds on the Spike X rasterizer + frame-hash harness (`gfx.rs`, #188) but
//! exercises the generalized **surface** API: off-screen `blyt_surface_h`
//! buffers, the reserved `BLYT_SCREEN`, the tier-1 serviced ops
//! (`blyt_surface_clear/pixel/rect_fill/line/blit`), and — later slices —
//! tier-2 acquire/release, draw()-only enforcement, and hybrid coherence.
//!
//! The acceptance oracle is the same derived golden the gfx suite uses
//! (`common::gfx`): the same logical frame drawn into a surface must hash
//! bit-identically on every leg and equal the reference rasterizer's output,
//! whether reached via gfx.* sugar or the surface API.

mod common;

use common::gfx;
use common::{
    CartProject, build_cart, build_debug_cart, build_lua_cart, require_lua_sdk,
    require_rust_riscv_target, require_sdk, require_wasm, run_cart_native_expect_fail,
    write_c_cart_project,
};
use common::{
    run_cart_libretro_with_env, run_cart_native_hostlua_frame_hash, run_cart_native_with_env,
    run_cart_wasm_with_env,
};

/// Build a C cart whose `blyt_cart_draw` runs `draw_body` once, then quits.
fn build_draw_cart(dir: &std::path::Path, draw_body: &str) -> std::path::PathBuf {
    let src = format!(
        r#"#include "blyt.h"
void blyt_cart_init(void) {{}}
void blyt_cart_update(void) {{ blyt_quit(); }}
void blyt_cart_draw(void) {{
{draw_body}}}
"#
    );
    write_c_cart_project(dir, &src);
    build_cart(dir)
}

/// Build a Rust cart whose `blyt_cart_draw` runs `draw_body` once, then quits.
fn build_rust_draw_cart(dir: &std::path::Path, draw_body: &str) -> std::path::PathBuf {
    let src = format!(
        r#"#![no_std]
#[no_mangle]
pub extern "C" fn blyt_cart_init() {{}}
#[no_mangle]
pub extern "C" fn blyt_cart_update() {{ blyt::quit(); }}
#[no_mangle]
pub extern "C" fn blyt_cart_draw() {{
{draw_body}}}
"#
    );
    CartProject::new().rust(&src).write(dir);
    build_cart(dir)
}

/// Draw()-only enforcement (#205, slice 5): a surface access outside `draw()`
/// is rejected.  In a release cart the rejection is a *defined, leg-identical*
/// behaviour — writes are dropped, a tier-2 acquire reads as cleared — never
/// UB; in a debug cart it is a hard error (a dev trap).  These tests pin both.

/// Out-of-phase WRITE is a no-op (release, every leg).  `update()` clears the
/// whole screen to colour 7 while the phase is UPDATE (not DRAW); if that write
/// leaked the screen would be 7 everywhere, but `draw()` only sets one corner
/// pixel, so the presented frame must be the blank (0) background with that
/// single pixel — the out-of-phase clear left no trace.  Hashes identically on
/// every emulated leg.
#[test]
fn surface_out_of_phase_write_is_noop_across_legs() {
    require_sdk();

    let src = r#"#include "blyt.h"
void blyt_cart_init(void) {}
void blyt_cart_update(void) {
  blyt_surface_clear(BLYT_SCREEN, 7); /* outside draw() -> no-op in release */
  blyt_quit();
}
void blyt_cart_draw(void) {
  blyt_surface_pixel(BLYT_SCREEN, 319, 239, 1); /* flips cart_has_drawn */
}
"#;
    let tmp = tempfile::tempdir().unwrap();
    let dir = tmp.path().join("surface-phase-write");
    write_c_cart_project(&dir, src);
    let cart = build_cart(&dir);

    // Screen is the blank background (the suppressed clear never ran), with the
    // one draw()-phase pixel at the far corner.
    let mut screen = vec![0u8; gfx::FRAME_W * gfx::FRAME_H];
    screen[(gfx::FRAME_H - 1) * gfx::FRAME_W + (gfx::FRAME_W - 1)] = 1;
    let expected = gfx::expected_hash_line(&screen);

    let env = [("BLYT_FRAME_HASH", "1")];
    run_cart_native_with_env(&cart, &env, &expected);
    run_cart_wasm_with_env(&cart, &env, &expected);
    run_cart_libretro_with_env(&cart, &env, &expected);
}

/// Out-of-phase READ is cleared (release, every leg).  Frame 0's `draw()` fills
/// the screen with colour 5 (real content).  Frame 1's `update()` — outside
/// `draw()` — acquires the screen and samples pixel 0; the draw()-only rule
/// means that acquire reads as *cleared*, so the sample is 0, not the real 5.
/// Frame 1's `draw()` paints that sampled value at (0,0) over a colour-3 fill,
/// so the presented frame is uniform 3 with a 0 at the origin.  If the read had
/// leaked the real content, (0,0) would be 5.
#[test]
fn surface_out_of_phase_read_is_cleared_across_legs() {
    require_sdk();

    let src = r#"#include "blyt.h"
static int g_n = 0;
static unsigned char g_read = 200; /* sentinel: neither 0 (cleared) nor 5 (real) */
void blyt_cart_init(void) {}
void blyt_cart_update(void) {
  if (g_n == 1) {
    blyt_lock_t lk;
    blyt_surface_acquire(BLYT_SCREEN, &lk); /* outside draw() -> reads cleared */
    g_read = lk.pixels[0];
    blyt_surface_release(&lk);
    blyt_quit();
  }
  g_n++;
}
void blyt_cart_draw(void) {
  if (g_n == 1) {
    blyt_surface_clear(BLYT_SCREEN, 5); /* frame 0: establish real content */
  } else {
    blyt_surface_clear(BLYT_SCREEN, 3);
    blyt_surface_pixel(BLYT_SCREEN, 0, 0, g_read);
  }
}
"#;
    let tmp = tempfile::tempdir().unwrap();
    let dir = tmp.path().join("surface-phase-read");
    write_c_cart_project(&dir, src);
    let cart = build_cart(&dir);

    // Frame 1: uniform colour 3, origin overwritten with the cleared (0) sample.
    let mut screen = vec![3u8; gfx::FRAME_W * gfx::FRAME_H];
    screen[0] = 0;
    let expected = gfx::expected_hash_line(&screen);

    // Two frames: the target hash is frame 1's, so run long enough to reach it.
    let env = [("BLYT_FRAME_HASH", "1")];
    run_cart_native_with_env(&cart, &env, &expected);
    run_cart_wasm_with_env(&cart, &env, &expected);
    run_cart_libretro_with_env(&cart, &env, &expected);
}

/// Out-of-phase surface access in a *debug* cart is a hard error (dev trap).
/// The same shape as the write-no-op test, but built `--debug`: the `.cart.info`
/// debug flag makes the runtime fault instead of silently no-op'ing, so the run
/// aborts (non-zero exit) rather than completing.  This pins that the release
/// no-op is a deliberate, debug-visible contract, not a silent swallow.
#[test]
fn surface_out_of_phase_access_faults_in_debug_cart() {
    require_sdk();

    let src = r#"#include "blyt.h"
void blyt_cart_init(void) {}
void blyt_cart_update(void) {
  blyt_surface_clear(BLYT_SCREEN, 7); /* outside draw() -> debug hard error */
  blyt_quit();
}
void blyt_cart_draw(void) {}
"#;
    let tmp = tempfile::tempdir().unwrap();
    let dir = tmp.path().join("surface-phase-debug-fault");
    write_c_cart_project(&dir, src);
    let cart = build_debug_cart(&dir);

    run_cart_native_expect_fail(&cart);
}

/// The tier-1 surface API drawn into `BLYT_SCREEN` is the gfx.* sugar's
/// canonical form: `blyt_surface_clear(BLYT_SCREEN, …)` and friends draw through
/// the *same* host-side rasterizer as `blyt_gfx_clear(…)`, so the torture frame
/// issued via the surface API must hash to the identical `common::gfx` golden on
/// every emulated leg. This pins that generalizing the fixed-framebuffer handlers
/// to a surface registry (slot 0 = screen) changed no pixel.
#[test]
fn surface_screen_torture_frame_hashes_identically_across_legs() {
    require_sdk();
    let ops = gfx::torture_frame();

    let tmp = tempfile::tempdir().unwrap();
    let cart = build_draw_cart(
        &tmp.path().join("surface-screen-torture"),
        &gfx::c_surface_draw_body(&ops, "BLYT_SCREEN"),
    );

    let expected = gfx::expected_hash_line(&gfx::render(&ops));
    let env = [("BLYT_FRAME_HASH", "1")];
    run_cart_native_with_env(&cart, &env, &expected);
    run_cart_wasm_with_env(&cart, &env, &expected);
    run_cart_libretro_with_env(&cart, &env, &expected);
}

/// End-to-end off-screen surface: create a blank surface, draw into it, then blit
/// it onto the screen.  The presented screen must hash bit-identically across all
/// legs and equal the reference (background cleared, then the rendered surface
/// composited at the blit offset) — proving create + tier-1 draw into an
/// off-screen buffer + blit-to-screen all compose deterministically.
#[test]
fn surface_offscreen_draw_then_blit_to_screen_hashes_identically_across_legs() {
    require_sdk();

    // The off-screen surface content and where it lands on the screen.
    const SW: i32 = 64;
    const SH: i32 = 48;
    const BX: i32 = 100;
    const BY: i32 = 80;
    let surf_ops = [
        gfx::Op::Clear(5),
        gfx::Op::Rect(10, 10, 20, 15, 9),
        gfx::Op::Line(0, 0, 63, 47, 12),
    ];
    let bg = 3u8;

    let mut body = format!("  blyt_surface_h s = blyt_surface_create({SW}, {SH});\n");
    body += &gfx::c_surface_draw_body(&surf_ops, "s");
    body += &format!("  blyt_surface_clear(BLYT_SCREEN, {bg});\n");
    body += &format!("  blyt_surface_blit(BLYT_SCREEN, s, {BX}, {BY});\n");

    let tmp = tempfile::tempdir().unwrap();
    let cart = build_draw_cart(&tmp.path().join("surface-offscreen-blit"), &body);

    // Reference: screen cleared to bg, then the rendered surface blitted at (BX,BY).
    let src = gfx::render_dims(&surf_ops, SW as usize, SH as usize);
    let mut screen = vec![bg; gfx::FRAME_W * gfx::FRAME_H];
    gfx::blit(
        &mut screen,
        gfx::FRAME_W,
        gfx::FRAME_H,
        &src,
        SW as usize,
        SH as usize,
        BX,
        BY,
    );
    let expected = gfx::expected_hash_line(&screen);

    let env = [("BLYT_FRAME_HASH", "1")];
    run_cart_native_with_env(&cart, &env, &expected);
    run_cart_wasm_with_env(&cart, &env, &expected);
    run_cart_libretro_with_env(&cart, &env, &expected);
}

/// Tier-1 ≡ tier-2 (#205 acceptance oracle): the torture frame drawn into the
/// screen *under a lock* — acquire, draw with the freestanding blyt_raster_*
/// primitives on the materialized buffer, release — must hash to the identical
/// golden the tier-1 serviced ops produce, on every leg.  This pins both the
/// per-pixel round-trip (copy-in/copy-out is byte-exact) and that in-lock drawing
/// shares the tier-1 rasterizer source.
#[test]
fn surface_lock_tier2_screen_equals_tier1_torture_across_legs() {
    require_sdk();
    let ops = gfx::torture_frame();

    let tmp = tempfile::tempdir().unwrap();
    let cart = build_draw_cart(
        &tmp.path().join("surface-lock-tier2"),
        &gfx::c_lock_draw_body(&ops, "BLYT_SCREEN", "lk"),
    );

    let expected = gfx::expected_hash_line(&gfx::render(&ops));
    let env = [("BLYT_FRAME_HASH", "1")];
    run_cart_native_with_env(&cart, &env, &expected);
    run_cart_wasm_with_env(&cart, &env, &expected);
    run_cart_libretro_with_env(&cart, &env, &expected);
}

/// Per-pixel round-trip through the copy-in/copy-out path: create an off-screen
/// surface, acquire it, write a deterministic pattern straight into lock.pixels
/// (raw, no rasterizer), release (flush), then blit to the screen.  The presented
/// screen must equal the reference (same pattern blitted over the background) on
/// every leg — proving raw per-pixel writes survive materialization byte-exactly.
#[test]
fn surface_lock_raw_per_pixel_roundtrips_across_legs() {
    require_sdk();

    const SW: i32 = 40;
    const SH: i32 = 30;
    let bg = 2u8;

    // Pattern p(x,y) = (x*7 + y*13) & 0xff, written per-pixel through the lock.
    let mut body = format!("  blyt_surface_clear(BLYT_SCREEN, {bg});\n");
    body += &format!("  blyt_surface_h s = blyt_surface_create({SW}, {SH});\n");
    body += "  blyt_lock_t lk;\n  blyt_surface_acquire(s, &lk);\n";
    body += "  for (int yy = 0; yy < lk.h; yy++)\n";
    body += "    for (int xx = 0; xx < lk.w; xx++)\n";
    body += "      lk.pixels[(long)yy * lk.stride + xx] = (unsigned char)((xx * 7 + yy * 13) & 0xff);\n";
    body += "  blyt_surface_release(&lk);\n";
    body += "  blyt_surface_blit(BLYT_SCREEN, s, 20, 15);\n";

    let tmp = tempfile::tempdir().unwrap();
    let cart = build_draw_cart(&tmp.path().join("surface-lock-raw"), &body);

    // Reference: build the src pattern, blit over the bg-cleared screen.
    let mut src = vec![0u8; (SW * SH) as usize];
    for yy in 0..SH {
        for xx in 0..SW {
            src[(yy * SW + xx) as usize] = ((xx * 7 + yy * 13) & 0xff) as u8;
        }
    }
    let mut screen = vec![bg; gfx::FRAME_W * gfx::FRAME_H];
    gfx::blit(
        &mut screen,
        gfx::FRAME_W,
        gfx::FRAME_H,
        &src,
        SW as usize,
        SH as usize,
        20,
        15,
    );
    let expected = gfx::expected_hash_line(&screen);

    let env = [("BLYT_FRAME_HASH", "1")];
    run_cart_native_with_env(&cart, &env, &expected);
    run_cart_wasm_with_env(&cart, &env, &expected);
    run_cart_libretro_with_env(&cart, &env, &expected);
}

/// A lock-view token is rejected once released (generation/lock-state check,
/// #205).  The cart draws colour X into the screen under a lock and releases it;
/// then it tampers — writes colour Y into the now-stale buffer and calls release
/// again with the same token.  The stale release must be a no-op, so the screen
/// stays X.  If the token were honoured the screen would become Y.
#[test]
fn surface_lock_stale_token_release_rejected_across_legs() {
    require_sdk();

    let x = 9u8;
    let y = 4u8;
    let mut body = String::from("  blyt_lock_t lk;\n  blyt_surface_acquire(BLYT_SCREEN, &lk);\n");
    body += &format!("  blyt_raster_clear(lk.pixels, lk.stride, lk.w, lk.h, {x});\n");
    body += "  blyt_surface_release(&lk);\n"; // screen = X, token now stale
    // Tamper: write Y into the stale region and try to flush it via the stale token.
    body += &format!("  blyt_raster_clear(lk.pixels, lk.stride, lk.w, lk.h, {y});\n");
    body += "  blyt_surface_release(&lk);\n"; // stale token -> no-op

    let tmp = tempfile::tempdir().unwrap();
    let cart = build_draw_cart(&tmp.path().join("surface-lock-stale"), &body);

    // Screen must be uniformly X — the stale release did not flush Y.
    let screen = vec![x; gfx::FRAME_W * gfx::FRAME_H];
    let expected = gfx::expected_hash_line(&screen);

    let env = [("BLYT_FRAME_HASH", "1")];
    run_cart_native_with_env(&cart, &env, &expected);
    run_cart_wasm_with_env(&cart, &env, &expected);
    run_cart_libretro_with_env(&cart, &env, &expected);
}

/// Exclusive-lock invariant (#207): a **blit reading a locked surface as its
/// source** is rejected, leg-identically.  While `s` is held by a tier-2 lock,
/// the lock owns it — every handle-path access is a defined no-op until release.
/// The cart clears the screen to a background, creates `s`, acquires it, fills
/// the *materialized lock buffer* with colour 7, then blits `s` onto the screen
/// while it is still locked.  With the invariant the blit no-ops, so the screen
/// stays pure background.  This is the one shape the emulated flush cannot mask:
/// without the fix the emulated leg blits the (still-blank) canonical `s` and the
/// native leg blits the lock's in-place colour-7 — two different frames, neither
/// the background.  Uniform rejection collapses them to the single golden.
#[test]
fn surface_blit_from_locked_src_rejected_across_legs() {
    require_sdk();

    let bg = 3u8;
    let mut body = format!("  blyt_surface_clear(BLYT_SCREEN, {bg});\n");
    body += "  blyt_surface_h s = blyt_surface_create(64, 48);\n";
    body += "  blyt_lock_t lk;\n  blyt_surface_acquire(s, &lk);\n";
    body += "  blyt_raster_clear(lk.pixels, lk.stride, lk.w, lk.h, 7);\n";
    body += "  blyt_surface_blit(BLYT_SCREEN, s, 100, 80); /* s locked -> rejected */\n";
    body += "  blyt_surface_release(&lk);\n";

    let tmp = tempfile::tempdir().unwrap();
    let cart = build_draw_cart(&tmp.path().join("surface-blit-locked-src"), &body);

    // The blit was rejected, so the screen is background only — the locked
    // surface never reaches it (with or without the flush that masks the write).
    let screen = vec![bg; gfx::FRAME_W * gfx::FRAME_H];
    let expected = gfx::expected_hash_line(&screen);

    let env = [("BLYT_FRAME_HASH", "1")];
    run_cart_native_with_env(&cart, &env, &expected);
    run_cart_wasm_with_env(&cart, &env, &expected);
    run_cart_libretro_with_env(&cart, &env, &expected);
}

/// Exclusive-lock invariant (#207): a **tier-1 op on the locked surface itself**
/// is rejected, leg-identically.  The cart acquires the screen, fills the lock
/// buffer with colour 9, then issues a tier-1 `clear(SCREEN, 4)` *while the screen
/// is still locked*, and releases (flushing the 9).  With the invariant the tier-1
/// clear no-ops on every leg, so the released frame is uniform 9.  This is the
/// native-discriminated case: the emulated flush masks the stray tier-1 write
/// either way (release overwrites the canonical buffer with the lock's 9), but a
/// bare-metal cart without the fix keeps the 4 (the lock exposes the canonical
/// buffer directly) — the QEMU gate is what catches that divergence.  Pinning the
/// golden here fixes the cross-leg identity the gate must match.
#[test]
fn surface_tier1_on_locked_screen_rejected_across_legs() {
    require_sdk();

    let a = 9u8; // in-lock content, flushed on release
    let b = 4u8; // stray tier-1 write on the locked screen — must be rejected
    let mut body = String::from("  blyt_lock_t lk;\n  blyt_surface_acquire(BLYT_SCREEN, &lk);\n");
    body += &format!("  blyt_raster_clear(lk.pixels, lk.stride, lk.w, lk.h, {a});\n");
    body += &format!("  blyt_surface_clear(BLYT_SCREEN, {b}); /* locked -> rejected */\n");
    body += "  blyt_surface_release(&lk);\n";

    let tmp = tempfile::tempdir().unwrap();
    let cart = build_draw_cart(&tmp.path().join("surface-tier1-locked"), &body);

    // Screen is uniform A: the stray tier-1 B never lands, the lock's A flushes.
    let screen = vec![a; gfx::FRAME_W * gfx::FRAME_H];
    let expected = gfx::expected_hash_line(&screen);

    let env = [("BLYT_FRAME_HASH", "1")];
    run_cart_native_with_env(&cart, &env, &expected);
    run_cart_wasm_with_env(&cart, &env, &expected);
    run_cart_libretro_with_env(&cart, &env, &expected);
}

/// A tier-1 op on a *locked* surface in a **debug** cart is a hard error (dev
/// trap), the debug-visible half of the #207 contract — the same relationship the
/// out-of-phase debug test pins for the draw()-only rule.  The release build's
/// no-op is a deliberate contract, not a silent swallow: built `--debug`, the
/// stray tier-1 clear on the locked screen faults instead of no-op'ing, so the
/// run aborts non-zero.
#[test]
fn surface_tier1_on_locked_surface_faults_in_debug_cart() {
    require_sdk();

    let src = r#"#include "blyt.h"
void blyt_cart_init(void) {}
void blyt_cart_update(void) { blyt_quit(); }
void blyt_cart_draw(void) {
  blyt_lock_t lk;
  blyt_surface_acquire(BLYT_SCREEN, &lk);
  blyt_surface_clear(BLYT_SCREEN, 4); /* locked -> debug hard error */
  blyt_surface_release(&lk);
}
"#;
    let tmp = tempfile::tempdir().unwrap();
    let dir = tmp.path().join("surface-locked-debug-fault");
    write_c_cart_project(&dir, src);
    let cart = build_debug_cart(&dir);

    run_cart_native_expect_fail(&cart);
}

/// A surface creation that would exceed the 16 MB memory budget (#158) returns
/// BLYT_HANDLE_NONE; drawing into and blitting from NONE are defined no-ops.  The
/// cart clears the screen to a background, tries an over-budget create, fills the
/// (would-be) surface, and blits it — so the presented screen is background-only.
/// The companion in-budget test above shows the same shape *does* composite, so
/// the contrast pins that the budget rejection is real, not an unrelated no-op.
#[test]
fn surface_over_budget_create_returns_none_across_legs() {
    require_sdk();

    // 8192x8192 = 64 MiB, four times the 16 MB budget, so create must return NONE
    // (the byte count is well within the dimension/overflow guard — it is the
    // budget that rejects it, not the size clamp).
    let bg = 3u8;
    let mut body = format!("  blyt_surface_clear(BLYT_SCREEN, {bg});\n");
    body += "  blyt_surface_h s = blyt_surface_create(8192, 8192);\n";
    body += "  blyt_surface_clear(s, 7);\n"; // no-op on NONE
    body += "  blyt_surface_blit(BLYT_SCREEN, s, 0, 0);\n"; // no-op on NONE

    let tmp = tempfile::tempdir().unwrap();
    let cart = build_draw_cart(&tmp.path().join("surface-over-budget"), &body);

    // Screen is background-only: the over-budget surface never materialized.
    let screen = vec![bg; gfx::FRAME_W * gfx::FRAME_H];
    let expected = gfx::expected_hash_line(&screen);

    let env = [("BLYT_FRAME_HASH", "1")];
    run_cart_native_with_env(&cart, &env, &expected);
    run_cart_wasm_with_env(&cart, &env, &expected);
    run_cart_libretro_with_env(&cart, &env, &expected);
}

/// The Lua tier-1 surface API (`blyt32.surface.*`) drawing the torture frame
/// into the screen must hash to the identical golden — proving the Lua binding
/// is pixel-exact with C/Rust across emulated-Lua (blytplay/libretro via
/// blyt32lua.c) and host-Lua (the wasm fast path).  Lua is tier-1 only (#205).
#[test]
fn surface_lua_screen_torture_hashes_identically_across_legs() {
    require_sdk();
    let ops = gfx::torture_frame();

    let tmp = tempfile::tempdir().unwrap();
    let project = tmp.path().join("surface-lua-torture");
    CartProject::new()
        .lua(&format!(
            "function init() end\n\
             function update() blyt.quit() end\n\
             function draw()\n{}end\n",
            gfx::lua_surface_draw_body(&ops, "blyt32.surface.SCREEN")
        ))
        .write(&project);
    let cart = build_cart(&project);

    let expected = gfx::expected_hash_line(&gfx::render(&ops));
    let env = [("BLYT_FRAME_HASH", "1")];
    run_cart_native_with_env(&cart, &env, &expected);
    run_cart_wasm_with_env(&cart, &env, &expected);
    run_cart_libretro_with_env(&cart, &env, &expected);
    run_cart_native_hostlua_frame_hash(&cart, &expected); // #231
}

/// #208 Stage 2: the Lua tier-2 per-pixel lock (`blyt32.surface.acquire` →
/// `lk:set`/`clear`/`rect_fill`/`line` → `lk:release`) drawing the torture frame
/// into the screen must hash to the identical golden on every leg — extending
/// the tier-1 ≡ tier-2 ≡ C guarantee (#205) to the Lua per-pixel path across
/// emulated-Lua (blyt32.c) and host-Lua (the wasm fast path). Red until acquire
/// and the lock userdata are bound in both Lua bindings.
#[test]
fn surface_lua_lock_tier2_screen_torture_hashes_identically_across_legs() {
    require_sdk();
    let ops = gfx::torture_frame();

    let tmp = tempfile::tempdir().unwrap();
    let project = tmp.path().join("surface-lua-lock-torture");
    CartProject::new()
        .lua(&format!(
            "function init() end\n\
             function update() blyt.quit() end\n\
             function draw()\n{}end\n",
            gfx::lua_lock_draw_body(&ops, "blyt32.surface.SCREEN", "lk")
        ))
        .write(&project);
    let cart = build_cart(&project);

    let expected = gfx::expected_hash_line(&gfx::render(&ops));
    let env = [("BLYT_FRAME_HASH", "1")];
    run_cart_native_with_env(&cart, &env, &expected);
    run_cart_wasm_with_env(&cart, &env, &expected);
    run_cart_libretro_with_env(&cart, &env, &expected);
    run_cart_native_hostlua_frame_hash(&cart, &expected); // #231
}

/// #208: `lk:get` reads back what `lk:set` wrote (per-pixel round-trip), and an
/// out-of-bounds read returns the defined cleared `0` in a release cart. The
/// frame is hashed on every leg so the guest (blyt32.c copy-in/out) and host-Lua
/// (direct pointer) read paths are proven pixel-identical.
#[test]
fn surface_lua_lock_get_roundtrip_across_legs() {
    require_sdk();
    // clear 3; write 42 at (10,10); copy it to (20,20) via get; OOB read → 0 at (30,30).
    let ops = [
        gfx::Op::Clear(3),
        gfx::Op::Pixel(10, 10, 42),
        gfx::Op::Pixel(20, 20, 42), // == lk:get(10,10)
        gfx::Op::Pixel(30, 30, 0),  // == lk:get(-1,-1), OOB → 0
    ];

    let tmp = tempfile::tempdir().unwrap();
    let project = tmp.path().join("surface-lua-lock-get");
    CartProject::new()
        .lua(
            "function init() end\n\
             function update() blyt.quit() end\n\
             function draw()\n\
             \x20 local lk = blyt32.surface.acquire(blyt32.surface.SCREEN)\n\
             \x20 lk:clear(3)\n\
             \x20 lk:set(10, 10, 42)\n\
             \x20 lk:set(20, 20, lk:get(10, 10))\n\
             \x20 lk:set(30, 30, lk:get(-1, -1))\n\
             \x20 lk:release()\n\
             end\n",
        )
        .write(&project);
    let cart = build_cart(&project);

    let expected = gfx::expected_hash_line(&gfx::render(&ops));
    let env = [("BLYT_FRAME_HASH", "1")];
    run_cart_native_with_env(&cart, &env, &expected);
    run_cart_wasm_with_env(&cart, &env, &expected);
    run_cart_libretro_with_env(&cart, &env, &expected);
    run_cart_native_hostlua_frame_hash(&cart, &expected); // #231
}

/// #208: using a lock after `lk:release()` is a defined no-op in a release cart
/// (never a crash). The post-release `set` must not land — the golden includes
/// only the pre-release pixel. Guards the released-flag check on every leg.
#[test]
fn surface_lua_lock_use_after_release_is_noop_across_legs() {
    require_sdk();
    let ops = [gfx::Op::Clear(3), gfx::Op::Pixel(5, 5, 7)];

    let tmp = tempfile::tempdir().unwrap();
    let project = tmp.path().join("surface-lua-lock-uar");
    CartProject::new()
        .lua(
            "function init() end\n\
             function update() blyt.quit() end\n\
             function draw()\n\
             \x20 local lk = blyt32.surface.acquire(blyt32.surface.SCREEN)\n\
             \x20 lk:clear(3)\n\
             \x20 lk:set(5, 5, 7)\n\
             \x20 lk:release()\n\
             \x20 lk:set(6, 6, 8)\n\
             end\n",
        )
        .write(&project);
    let cart = build_cart(&project);

    let expected = gfx::expected_hash_line(&gfx::render(&ops));
    let env = [("BLYT_FRAME_HASH", "1")];
    run_cart_native_with_env(&cart, &env, &expected);
    run_cart_wasm_with_env(&cart, &env, &expected);
    run_cart_libretro_with_env(&cart, &env, &expected);
    run_cart_native_hostlua_frame_hash(&cart, &expected); // #231
}

/// #208: in a DEBUG cart, an out-of-bounds `lk:set` is a hard Lua error — not the
/// release no-op — surfaced through Lua's normal error path (and, in a debug
/// session, pausing the DAP debugger). Headless, the raised error is logged to
/// the console, so we assert its message appears. This pins that the release
/// no-op is a deliberate, debug-visible contract, and that the guest read
/// cart_is_debug == 1 from the host-published runtime-flags block. (Unlike the
/// host-side phase gate, a guest Lua error follows Lua's log-and-continue path
/// rather than aborting the process — see call_global in blyt32lua.c.)
#[test]
fn surface_lua_lock_oob_errors_in_debug_cart() {
    require_sdk();
    let tmp = tempfile::tempdir().unwrap();
    let project = tmp.path().join("surface-lua-lock-oob-debug");
    CartProject::new()
        .lua(
            "function init() end\n\
             function update() blyt.quit() end\n\
             function draw()\n\
             \x20 local lk = blyt32.surface.acquire(blyt32.surface.SCREEN)\n\
             \x20 lk:set(1000, 1000, 7)\n\
             end\n",
        )
        .write(&project);
    let cart = build_debug_cart(&project);
    run_cart_native_with_env(&cart, &[], "out of bounds");
}

/// The Lua tier-1 surface API end-to-end: create a blank off-screen surface,
/// draw into it, clear the screen, and blit the surface onto it.  The presented
/// screen must hash identically across every leg and equal the reference —
/// proving Lua `blyt32.surface.create`/draw/`blit` compose deterministically on
/// the emulated-Lua registry (ECALL) and the host-Lua pool (wasm) alike (#205).
#[test]
fn surface_lua_offscreen_draw_then_blit_hashes_identically_across_legs() {
    require_sdk();

    const SW: i32 = 64;
    const SH: i32 = 48;
    const BX: i32 = 100;
    const BY: i32 = 80;
    let surf_ops = [
        gfx::Op::Clear(5),
        gfx::Op::Rect(10, 10, 20, 15, 9),
        gfx::Op::Line(0, 0, 63, 47, 12),
    ];
    let bg = 3u8;

    let mut draw = format!("  local s = blyt32.surface.create({SW}, {SH})\n");
    draw += &gfx::lua_surface_draw_body(&surf_ops, "s");
    draw += &format!("  blyt32.surface.clear(blyt32.surface.SCREEN, {bg})\n");
    draw += &format!("  blyt32.surface.blit(blyt32.surface.SCREEN, s, {BX}, {BY})\n");

    let tmp = tempfile::tempdir().unwrap();
    let project = tmp.path().join("surface-lua-offscreen");
    CartProject::new()
        .lua(&format!(
            "function init() end\n\
             function update() blyt.quit() end\n\
             function draw()\n{draw}end\n"
        ))
        .write(&project);
    let cart = build_cart(&project);

    let src = gfx::render_dims(&surf_ops, SW as usize, SH as usize);
    let mut screen = vec![bg; gfx::FRAME_W * gfx::FRAME_H];
    gfx::blit(
        &mut screen,
        gfx::FRAME_W,
        gfx::FRAME_H,
        &src,
        SW as usize,
        SH as usize,
        BX,
        BY,
    );
    let expected = gfx::expected_hash_line(&screen);

    let env = [("BLYT_FRAME_HASH", "1")];
    run_cart_native_with_env(&cart, &env, &expected);
    run_cart_wasm_with_env(&cart, &env, &expected);
    run_cart_libretro_with_env(&cart, &env, &expected);
    run_cart_native_hostlua_frame_hash(&cart, &expected); // #231
}

/// #208 cross-half screen-lock coherence: while the host-Lua half holds a tier-2
/// lock on the SCREEN, a tier-1 op the native half issues on the screen must be
/// REJECTED on every leg — the #207 exclusive-lock invariant, now spanning both
/// halves of a WASM hybrid through the one session registry (#210). The frame
/// hashes identically across legs and equals the golden WITHOUT the rejected op:
/// on the fast path the Lua acquire marks the session slot locked, so
/// cart_run.c's lock gate rejects the native op exactly as on the single-registry
/// (emulated/native) legs. This is the case #210 was landed to make coherent.
#[test]
fn surface_hybrid_lua_screen_lock_rejects_native_tier1_across_legs() {
    require_sdk();
    require_lua_sdk();
    require_wasm();

    let c_src = "#include \"blyt.h\"\n\
                 BLYT_LUA_EXPORT_VOID(native_rect) {\n\
                 \x20 blyt_surface_rect_fill(BLYT_SCREEN, 50, 50, 20, 20, 9);\n\
                 }\n";
    let lua_src = "function init() end\n\
                   function update() blyt.quit() end\n\
                   function draw()\n\
                   \x20 local lk = blyt32.surface.acquire(blyt32.surface.SCREEN)\n\
                   \x20 lk:clear(3)\n\
                   \x20 lk:set(10, 10, 7)\n\
                   \x20 native_rect()\n\
                   \x20 lk:release()\n\
                   end\n";
    let tmp = tempfile::tempdir().unwrap();
    let dir = tmp.path().join("surface-hybrid-lock-reject");
    CartProject::new().c(c_src).lua(lua_src).write(&dir);
    let cart = build_lua_cart(&dir);

    // Golden = only the Lua lock's draws; the native rect is rejected on every leg.
    let ops = [gfx::Op::Clear(3), gfx::Op::Pixel(10, 10, 7)];
    let expected = gfx::expected_hash_line(&gfx::render(&ops));
    let env = [("BLYT_FRAME_HASH", "1")];
    run_cart_native_with_env(&cart, &env, &expected);
    run_cart_wasm_with_env(&cart, &env, &expected);
    run_cart_libretro_with_env(&cart, &env, &expected);
}

/// Hybrid coherence with the tier-2 lock crossing the Lua/native boundary
/// (#205). A hybrid cart draws the torture frame in two sequential halves into
/// the ONE screen surface: the native half acquires the screen (tier-2), draws
/// its slice with the in-lock primitives, and releases — flushing to the shared
/// canonical buffer; then the host-Lua half draws the rest via the tier-1
/// surface API. The combined frame must hash bit-identically to the same cart
/// run fully-emulated and to the single-buffer golden, across the emulated trio.
/// This is the coherence claim: a tier-2 lock composes with tier-1 across the
/// bridge because release publishes to the one buffer both halves share.
#[test]
fn surface_hybrid_native_tier2_lua_tier1_coheres_across_legs() {
    require_sdk();
    require_lua_sdk();
    require_wasm();
    let ops = gfx::torture_frame();
    let (native_ops, lua_ops) = ops.split_at(8);

    let c_src = format!(
        "#include \"blyt.h\"\n\
         BLYT_LUA_EXPORT_VOID(native_draw) {{\n{}}}\n",
        gfx::c_lock_draw_body(native_ops, "BLYT_SCREEN", "lk")
    );
    let lua_src = format!(
        "function init() end\n\
         function update() blyt.quit() end\n\
         function draw()\n  native_draw()\n{}end\n",
        gfx::lua_surface_draw_body(lua_ops, "blyt32.surface.SCREEN")
    );
    let tmp = tempfile::tempdir().unwrap();
    let dir = tmp.path().join("surface-hybrid-tier2");
    CartProject::new().c(&c_src).lua(&lua_src).write(&dir);
    let cart = build_lua_cart(&dir);

    let expected = gfx::expected_hash_line(&gfx::render(&ops));
    let env = [("BLYT_FRAME_HASH", "1")];
    run_cart_native_with_env(&cart, &env, &expected);
    run_cart_wasm_with_env(&cart, &env, &expected);
    run_cart_libretro_with_env(&cart, &env, &expected);
}

/// A lock-view token is inert once it crosses the Lua/native boundary (#205).
/// The native half acquires the screen, draws colour X under the lock, releases,
/// and returns the (now spent) lock-view token to Lua. Lua passes that token to
/// a tier-1 surface op (`blyt32.surface.clear(tok, Y)`) — but a lock-view is a
/// distinct handle kind, so the runtime's classify-at-entry rejects it and the
/// op no-ops. The screen must stay X (never Y) on every leg: the live lock never
/// marshals (its buffer pointer is meaningless across the bridge) and the token
/// that does cross fails the next op's kind check.
#[test]
fn surface_lockview_token_across_bridge_rejected_across_legs() {
    require_sdk();
    require_lua_sdk();
    require_wasm();
    let x = 6u8;
    let y = 11u8;

    let c_src = format!(
        "#include \"blyt.h\"\n\
         BLYT_LUA_EXPORT_I32(native_lock_and_leak_token, int32_t unused) {{\n\
         \x20 (void)unused;\n\
         \x20 blyt_lock_t lk;\n\
         \x20 blyt_surface_acquire(BLYT_SCREEN, &lk);\n\
         \x20 blyt_raster_clear(lk.pixels, lk.stride, lk.w, lk.h, {x});\n\
         \x20 blyt_surface_release(&lk);\n\
         \x20 return (int32_t)lk.token;\n\
         }}\n"
    );
    let lua_src = format!(
        "function init() end\n\
         function update() blyt.quit() end\n\
         function draw()\n\
         \x20 local tok = native_lock_and_leak_token(0)\n\
         \x20 blyt32.surface.clear(tok, {y})\n\
         end\n"
    );
    let tmp = tempfile::tempdir().unwrap();
    let dir = tmp.path().join("surface-lockview-bridge");
    CartProject::new().c(&c_src).lua(&lua_src).write(&dir);
    let cart = build_lua_cart(&dir);

    // The token is a LOCKVIEW handle; a tier-1 op on it fails the kind check and
    // no-ops, so the screen stays uniformly X — Y never lands.
    let screen = vec![x; gfx::FRAME_W * gfx::FRAME_H];
    let expected = gfx::expected_hash_line(&screen);
    let env = [("BLYT_FRAME_HASH", "1")];
    run_cart_native_with_env(&cart, &env, &expected);
    run_cart_wasm_with_env(&cart, &env, &expected);
    run_cart_libretro_with_env(&cart, &env, &expected);
}

/// The Rust SDK surface API (`blyt::gfx`) draws the torture frame into the
/// screen via the tier-1 methods and must hash to the identical `common::gfx`
/// golden every other leg produces — proving the Rust bindings are pixel-exact
/// with the C path (#205, the "C + Rust" scope).
#[test]
fn surface_rust_screen_torture_hashes_identically_across_legs() {
    require_sdk();
    require_rust_riscv_target();
    let ops = gfx::torture_frame();

    let tmp = tempfile::tempdir().unwrap();
    let cart = build_rust_draw_cart(
        &tmp.path().join("surface-rust-torture"),
        &gfx::rust_surface_draw_body(&ops, "blyt::gfx::SCREEN"),
    );

    let expected = gfx::expected_hash_line(&gfx::render(&ops));
    let env = [("BLYT_FRAME_HASH", "1")];
    run_cart_native_with_env(&cart, &env, &expected);
    run_cart_wasm_with_env(&cart, &env, &expected);
    run_cart_libretro_with_env(&cart, &env, &expected);
}

/// The Rust `SurfaceLock` guard (tier-2): create an off-screen surface, draw into
/// it under a lock (in-lock primitives on the materialized buffer), let the guard
/// drop (which flushes), then blit to the screen. The presented screen must equal
/// the reference on every leg — exercising acquire, the in-lock rasterizer
/// helpers, and Drop-as-release from Rust.
#[test]
fn surface_rust_lock_offscreen_blit_hashes_identically_across_legs() {
    require_sdk();
    require_rust_riscv_target();

    const SW: i32 = 64;
    const SH: i32 = 48;
    const BX: i32 = 100;
    const BY: i32 = 80;
    let surf_ops = [
        gfx::Op::Clear(5),
        gfx::Op::Rect(10, 10, 20, 15, 9),
        gfx::Op::Line(0, 0, 63, 47, 12),
    ];
    let bg = 3u8;

    let mut body = format!("    let s = blyt::gfx::Surface::create({SW}, {SH}).unwrap();\n");
    body += "    {\n        let mut lk = s.acquire().unwrap();\n";
    body += "        lk.clear(5);\n";
    body += "        lk.rect_fill(10, 10, 20, 15, 9);\n";
    body += "        lk.line(0, 0, 63, 47, 12);\n";
    body += "    }\n"; // drop flushes
    body += &format!("    blyt::gfx::SCREEN.clear({bg});\n");
    body += &format!("    blyt::gfx::SCREEN.blit(s, {BX}, {BY});\n");

    let tmp = tempfile::tempdir().unwrap();
    let cart = build_rust_draw_cart(&tmp.path().join("surface-rust-lock"), &body);

    let src = gfx::render_dims(&surf_ops, SW as usize, SH as usize);
    let mut screen = vec![bg; gfx::FRAME_W * gfx::FRAME_H];
    gfx::blit(
        &mut screen,
        gfx::FRAME_W,
        gfx::FRAME_H,
        &src,
        SW as usize,
        SH as usize,
        BX,
        BY,
    );
    let expected = gfx::expected_hash_line(&screen);

    let env = [("BLYT_FRAME_HASH", "1")];
    run_cart_native_with_env(&cart, &env, &expected);
    run_cart_wasm_with_env(&cart, &env, &expected);
    run_cart_libretro_with_env(&cart, &env, &expected);
}

/// FLAGSHIP cross-registry coherence (#210): a hybrid cart where the **Lua** half
/// creates an *off-screen* surface, draws a known pattern into it, and passes the
/// surface **handle across the bridge** to a **C** export that blits it onto the
/// screen. The presented frame must hash bit-identically on every leg and equal
/// the `common::gfx` golden.
///
/// This is the divergence the WASM fast path introduces: a hybrid runs its Lua
/// half on the host-Lua fast path (its own surface pool) and its native half in
/// the rv32 session (a separate registry), so the Lua-created off-screen handle
/// resolves to a *different / invalid* slot when C blits it — the pattern never
/// lands and the WASM frame diverges from the single-registry legs. Every other
/// leg runs the hybrid in one registry and is already coherent. Unifying the
/// fast path onto the session's registry (with a once-per-real-frame reap so the
/// C-export trampoline does not destroy the surface mid-frame) makes the WASM
/// leg match. Red on the WASM leg without the fix.
#[test]
fn surface_hybrid_lua_offscreen_handle_blit_by_c_coheres_across_legs() {
    require_sdk();
    require_lua_sdk();
    require_wasm();

    const SW: i32 = 120;
    const SH: i32 = 80;
    const BX: i32 = 40;
    const BY: i32 = 30;
    let bg = 3u8;
    let surf_ops = [
        gfx::Op::Clear(5),
        gfx::Op::Rect(10, 10, 40, 20, 7),
        gfx::Op::Line(0, 0, 119, 79, 9),
        gfx::Op::Pixel(60, 40, 11),
    ];

    // C half: blit the Lua-created surface (handle passed across the bridge as a
    // scalar) onto the screen.  The SURFACE handle keeps bit 31 clear, so it
    // round-trips through the i32 bridge arg unchanged.
    let c_src = format!(
        "#include \"blyt.h\"\n\
         BLYT_LUA_EXPORT_I32(blit_surf_to_screen, int32_t surf) {{\n\
         \x20 blyt_surface_blit(BLYT_SCREEN, (blyt_surface_h)(uint32_t)surf, {BX}, {BY});\n\
         \x20 return 0;\n\
         }}\n"
    );
    let lua_src = format!(
        "function init() end\n\
         function update() blyt.quit() end\n\
         function draw()\n\
         \x20 blyt32.surface.clear(blyt32.surface.SCREEN, {bg})\n\
         \x20 local surf = blyt32.surface.create({SW}, {SH})\n\
         {}\
         \x20 blit_surf_to_screen(surf)\n\
         end\n",
        gfx::lua_surface_draw_body(&surf_ops, "surf")
    );

    let tmp = tempfile::tempdir().unwrap();
    let dir = tmp.path().join("surface-hybrid-offscreen-handle");
    CartProject::new().c(&c_src).lua(&lua_src).write(&dir);
    let cart = build_lua_cart(&dir);

    let src = gfx::render_dims(&surf_ops, SW as usize, SH as usize);
    let mut screen = vec![bg; gfx::FRAME_W * gfx::FRAME_H];
    gfx::blit(
        &mut screen,
        gfx::FRAME_W,
        gfx::FRAME_H,
        &src,
        SW as usize,
        SH as usize,
        BX,
        BY,
    );
    let expected = gfx::expected_hash_line(&screen);

    let env = [("BLYT_FRAME_HASH", "1")];
    run_cart_native_with_env(&cart, &env, &expected);
    run_cart_wasm_with_env(&cart, &env, &expected);
    run_cart_libretro_with_env(&cart, &env, &expected);
}
