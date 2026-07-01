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
