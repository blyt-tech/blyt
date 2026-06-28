//! Spike X — host-side 2D rasterizer: framebuffer mechanism + determinism.
//!
//! This suite proves the two load-bearing assumptions before the ~50-entry
//! graphics surface is built (issue #188):
//!   Q1 — a single `acquire`/`present` + ECALL-primitive contract spans the
//!        emulated, host-Lua, and native execution models, and
//!   Q2 — one integer rasterizer source hashes bit-identically across every
//!        compile target.
//!
//! These tests cover the three *emulated* legs (native blytplay / wasm /
//! libretro), all running the same host runtime.  The fourth Q2 target — the
//! bare-metal native leg, where the RV32-compiled rasterizer runs without an
//! emulator — is asserted by the QEMU gate in `native_qemu.rs` against the same
//! `common::gfx` golden.

mod common;

use common::gfx;
use common::{
    CartProject, build_cart, require_sdk, run_cart_libretro_with_env, run_cart_native_with_env,
    run_cart_wasm_with_env, write_c_cart_project,
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

/// Dump frame 0 (XRGB8888) of a cart via `blytplay --headless --dump-frame0`.
fn dump_frame0(cart: &std::path::Path) -> Vec<u8> {
    use assert_cmd::Command;
    let tmp = tempfile::NamedTempFile::new().unwrap();
    let path = tmp.path().to_str().unwrap().to_string();
    Command::new(common::blytplay())
        .args(["--headless", "--dump-frame0", &path, cart.to_str().unwrap()])
        .assert()
        .success();
    std::fs::read(tmp.path()).unwrap()
}

fn is_uniform(frame: &[u8]) -> bool {
    let first = &frame[0..4];
    frame.chunks_exact(4).all(|px| px == first)
}

/// `blyt_gfx_clear` fills the whole framebuffer with one colour and, by setting
/// `cart_has_drawn`, displaces the (non-uniform) PM5544 test card.
#[test]
fn gfx_clear_fills_framebuffer_and_displaces_testcard() {
    require_sdk();
    let tmp = tempfile::tempdir().unwrap();

    // Probe cart: clear the screen to a fixed palette index.
    let probe = build_draw_cart(&tmp.path().join("gfx-clear"), "  blyt_gfx_clear(7);\n");
    let probe_frame = dump_frame0(&probe);

    // Baseline: a cart that never draws — frame 0 is the PM5544 test card.
    let idle = build_draw_cart(&tmp.path().join("gfx-idle"), "");
    let testcard_frame = dump_frame0(&idle);

    assert_eq!(
        probe_frame.len(),
        gfx::FRAME_BYTES,
        "frame is 320x240 XRGB8888"
    );
    assert!(
        is_uniform(&probe_frame),
        "blyt_gfx_clear should fill the framebuffer uniformly"
    );
    assert!(
        !is_uniform(&testcard_frame),
        "sanity: the test card baseline must be non-uniform"
    );
    assert_ne!(
        probe_frame, testcard_frame,
        "drawing must displace the test card (cart_has_drawn)"
    );
}

/// Q2 — the load-bearing determinism assumption.  One integer rasterizer source
/// compiled into the host runtime (native blytplay leg), the wasm module (wasm
/// leg), and the embedded libretro core hashes the 320x240 paletted framebuffer
/// **bit-identically across all three legs**, and every hash matches the
/// reference rasterizer's golden.  The torture frame exercises every primitive
/// plus off-screen / zero-size / negative / overflow edge cases, so this also
/// pins per-primitive pixel coverage.
///
/// (The native QEMU leg — the fourth target — asserts the same golden in
/// `native_qemu.rs`; this is the emulated-trio portion of Q2.)
#[test]
fn gfx_torture_frame_hashes_identically_across_legs() {
    require_sdk();
    let ops = gfx::torture_frame();

    let tmp = tempfile::tempdir().unwrap();
    let cart = build_draw_cart(&tmp.path().join("gfx-torture"), &gfx::c_draw_body(&ops));

    let expected = gfx::expected_hash_line(&gfx::render(&ops));
    let env = [("BLYT_FRAME_HASH", "1")];
    run_cart_native_with_env(&cart, &env, &expected);
    run_cart_wasm_with_env(&cart, &env, &expected);
    run_cart_libretro_with_env(&cart, &env, &expected);
}

/// Q1 — the acquire/present raw-framebuffer contract.  `blyt_gfx_acquire()`
/// returns a guest-VA pointer to a runtime-reserved framebuffer region; the cart
/// writes a deterministic per-pixel pattern straight into it (no per-pixel ECALL)
/// and `blyt_gfx_present()` flushes the region into the host framebuffer and
/// displaces the test card.  The presented frame must hash bit-identically across
/// all three emulated legs and match the reference pattern's golden — proving the
/// one acquire/present mechanism is consistent across execution models and the
/// region round-trips byte-exactly.
#[test]
fn gfx_acquire_present_raw_framebuffer_hashes_identically_across_legs() {
    require_sdk();

    let tmp = tempfile::tempdir().unwrap();
    let cart = build_draw_cart(
        &tmp.path().join("gfx-raw-present"),
        &gfx::raw_present_c_draw(),
    );

    let expected = gfx::expected_hash_line(&gfx::raw_pattern_frame());
    let env = [("BLYT_FRAME_HASH", "1")];
    run_cart_native_with_env(&cart, &env, &expected);
    run_cart_wasm_with_env(&cart, &env, &expected);
    run_cart_libretro_with_env(&cart, &env, &expected);
}

/// Q1 across execution models — the **host-Lua fast path** (the third model).  A
/// pure-Lua cart draws the torture frame via `blyt32.gfx.*`; on wasm it runs on
/// the host Lua VM with no emulator (the fast path's own `blyt32.gfx` binding +
/// shared rasterizer), while blytplay and libretro run the same cart through the
/// emulator (the guest `blyt32lua.c` binding → host primitives).  All three must
/// emit the SAME `[blyt:fbhash]` golden the C carts produce — proving one gfx
/// contract spans emulated-C, emulated-Lua, host-Lua (and, via the QEMU gate,
/// native), and that the host-Lua fast path stays pixel-identical to the
/// emulated path it shadows.
#[test]
fn gfx_torture_frame_lua_hashes_identically_across_legs() {
    require_sdk();
    let ops = gfx::torture_frame();

    let tmp = tempfile::tempdir().unwrap();
    let project = tmp.path().join("gfx-lua-torture");
    CartProject::new()
        .lua(&format!(
            "function init() end\n\
             function update() blyt.quit() end\n\
             function draw()\n{}end\n",
            gfx::lua_draw_body(&ops)
        ))
        .write(&project);
    let cart = build_cart(&project);

    let expected = gfx::expected_hash_line(&gfx::render(&ops));
    let env = [("BLYT_FRAME_HASH", "1")];
    run_cart_native_with_env(&cart, &env, &expected);
    run_cart_wasm_with_env(&cart, &env, &expected);
    run_cart_libretro_with_env(&cart, &env, &expected);
}
