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
use common::{build_cart, require_sdk, write_c_cart_project};
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
