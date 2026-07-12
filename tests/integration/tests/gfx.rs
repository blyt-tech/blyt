//! Spike X — host-side 2D rasterizer: framebuffer mechanism + determinism.
//!
//! This suite proves the two load-bearing assumptions before the ~50-entry
//! graphics surface is built (issue #188):
//!   Q1 — a single `acquire`/`present` + ECALL-primitive contract spans the
//!        emulated, host-Lua, and native execution models, and
//!   Q2 — one integer rasterizer source hashes bit-identically across every
//!        compile target.
//!
//! These tests cover the three host-runtime legs (native blytplay / wasm /
//! libretro), all running the same host runtime — C carts under rv32emu, Lua
//! carts on the host-Lua path (the default on non-RISC-V hosts after #236,
//! ADR-0136).  The fourth Q2 target — the bare-metal native leg, where the
//! RV32-compiled rasterizer runs without an emulator — is asserted by the QEMU
//! gate in `native_qemu.rs` against the same `common::gfx` golden.

mod common;

use common::gfx;
use common::{
    CartProject, build_cart, build_lua_cart, require_lua_sdk, require_sdk, require_wasm,
    run_cart_all_legs_frame_hash, write_c_cart_project,
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
    run_cart_all_legs_frame_hash(&cart, &expected);
}

/// `blyt_gfx_palette_set` (issue #201): the new op must be serviced without
/// trapping on every leg, and — since `[blyt:fbhash]` hashes palette *indices*,
/// not the palette's RGB content — loading a different built-in palette before
/// drawing the SAME torture frame must hash identically to the palette-less
/// run. (Cross-leg *color* parity is a palette-sensitive oracle deferred to
/// #204; this pins that the op round-trips through every leg's dispatch path
/// without perturbing the index-only hash the emulated trio already proves.)
#[test]
fn gfx_palette_set_does_not_perturb_index_hash_across_legs() {
    require_sdk();
    let ops = gfx::torture_frame();

    let mut draw_body = "  blyt_gfx_palette_set(BLYT_PALETTE_VGA);\n".to_string();
    draw_body += &gfx::c_draw_body(&ops);

    let tmp = tempfile::tempdir().unwrap();
    let cart = build_draw_cart(&tmp.path().join("gfx-palette-set"), &draw_body);

    let expected = gfx::expected_hash_line(&gfx::render(&ops));
    run_cart_all_legs_frame_hash(&cart, &expected);
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
    run_cart_all_legs_frame_hash(&cart, &expected);
}

/// Q1 across execution models — the **host-Lua path** (ADR-0136).  A pure-Lua
/// cart draws the torture frame via `blyt32.gfx.*`.  After #236 retired the
/// emulated RV32 Lua VM as a shipped path, all three host-runtime legs run this
/// cart on the host Lua VM with no emulator: blytplay and the libretro core each
/// rasterize into the native runner's own framebuffer, and wasm runs the host-Lua
/// fast path.  All three must emit the SAME `[blyt:fbhash]` golden the C carts
/// produce — proving the host-Lua gfx bindings stay pixel-identical to the
/// reference rasterizer (and, via the QEMU native gate, to real RV32 hardware).
#[test]
fn gfx_torture_frame_lua_hashes_identically_across_legs() {
    require_sdk();
    require_lua_sdk();
    require_wasm();
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
    run_cart_all_legs_frame_hash(&cart, &expected);
}

/// Build a Lua + native-C **hybrid** cart whose torture frame is split across
/// both halves: the native C lib draws `ops[..split]` via `blyt_gfx_*`, then the
/// Lua `draw()` calls that export and draws `ops[split..]` via `blyt32.gfx.*`.
/// Both halves must land in ONE framebuffer, so the combined frame hashes to
/// `render(ops)` — the #193 coherence proof.
fn build_gfx_hybrid_cart(
    dir: &std::path::Path,
    ops: &[gfx::Op],
    split: usize,
) -> std::path::PathBuf {
    let (native_ops, lua_ops) = ops.split_at(split);
    let c_src = format!(
        "#include \"blyt.h\"\n\
         BLYT_LUA_EXPORT_VOID(native_draw) {{\n{}}}\n",
        gfx::c_draw_body(native_ops)
    );
    let lua_src = format!(
        "function init() end\n\
         function update() blyt.quit() end\n\
         function draw()\n  native_draw()\n{}end\n",
        gfx::lua_draw_body(lua_ops)
    );
    CartProject::new().c(&c_src).lua(&lua_src).write(dir);
    build_lua_cart(dir)
}

/// The #193 bug cell + hybrid parity.  A hybrid cart draws the torture frame from
/// **both** halves — native (`blyt_gfx_*`) and host-Lua (`blyt32.gfx.*`).  On wasm
/// the two halves live in two address spaces (Lua host-side, native in rv32emu per
/// ADR-0130); before the fix the host-Lua half drew into a separate `g_lua_pixels`
/// and the native half's output was silently dropped.  After unifying onto the
/// session's canonical framebuffer the combined frame must hash **bit-identically**
/// to the same cart run fully-emulated — and to the single-buffer golden — across
/// the emulated trio (blytplay / wasm / libretro).
#[test]
fn gfx_hybrid_both_halves_hash_identically_across_legs() {
    require_sdk();
    require_lua_sdk();
    require_wasm();
    let ops = gfx::torture_frame();

    let tmp = tempfile::tempdir().unwrap();
    // Split so each half draws a non-trivial slice (native: clear + rects + a
    // pixel; Lua: the remaining pixels + every line primitive).
    let cart = build_gfx_hybrid_cart(&tmp.path().join("gfx-hybrid"), &ops, 8);

    let expected = gfx::expected_hash_line(&gfx::render(&ops));
    run_cart_all_legs_frame_hash(&cart, &expected);
}

/// Build a hybrid cart whose native half uses the raw `acquire`/`present` path
/// (writing [`gfx::raw_pattern_frame`] straight into the runtime-reserved
/// region); the Lua half only drives the frame.  On wasm the native half is
/// emulated, so its `present` lands in `session->pixels[]` — exactly the buffer
/// the host-Lua present path dropped before #193.
fn build_gfx_hybrid_raw_cart(dir: &std::path::Path) -> std::path::PathBuf {
    let c_src = format!(
        "#include \"blyt.h\"\n\
         BLYT_LUA_EXPORT_VOID(native_present) {{\n{}}}\n",
        gfx::raw_present_c_draw()
    );
    let lua_src = "function init() end\n\
                   function update() blyt.quit() end\n\
                   function draw() native_present() end\n";
    CartProject::new().c(&c_src).lua(lua_src).write(dir);
    build_lua_cart(dir)
}

/// Hybrid `acquire`/`present` from the native half.  A hybrid cart whose native
/// code acquires the raw framebuffer and presents a deterministic pattern must
/// hash identically across the emulated trio — proving the native half's
/// `acquire`/`present` (the path most clearly dropped on wasm before #193) now
/// reaches the unified framebuffer the wasm present path emits.
#[test]
fn gfx_hybrid_native_acquire_present_hashes_identically_across_legs() {
    require_sdk();
    require_lua_sdk();
    require_wasm();

    let tmp = tempfile::tempdir().unwrap();
    let cart = build_gfx_hybrid_raw_cart(&tmp.path().join("gfx-hybrid-raw"));

    let expected = gfx::expected_hash_line(&gfx::raw_pattern_frame());
    run_cart_all_legs_frame_hash(&cart, &expected);
}
