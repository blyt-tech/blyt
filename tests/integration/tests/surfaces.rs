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
    CartProject, build_cart, build_debug_cart, require_rust_riscv_target, require_sdk,
    run_cart_native_expect_fail, write_c_cart_project,
};
use common::{run_cart_libretro_with_env, run_cart_native_with_env, run_cart_wasm_with_env};

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
