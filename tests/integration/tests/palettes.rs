//! Palette bundling + config-driven pre-init auto-load (issue #201, ADR-0042/0088).
//!
//! Exercises the full pipeline end-to-end: `palettes: default: <name>` in
//! blyt.config.yaml -> devtool validation/encoding -> .cart.config FlatBuffer
//! section -> cart_load.c -> cart_run.c's session-create palette resolution.
//! The exact 256-entry byte content of each built-in is already pinned by the
//! C unit test (tests/unit/test_palettes.c); this test checks distinguishing
//! bytes to confirm the RIGHT table reaches the session through the real
//! build+load path, not the resolver in isolation.

mod common;

use assert_cmd::Command;
use common::gfx;
use common::{
    CartProject, build_cart, build_lua_cart, dump_frame0_native, dump_frame0_wasm, repo_root,
    require_libretro_core, require_lua_sdk, require_sdk, require_test_session_api, require_wasm,
    run_cart_libretro_with_env, run_cart_native_with_env, run_cart_wasm_with_env, sdk_dir,
    test_session_api,
};
use tempfile::TempDir;

const PLAIN_CART_C: &str = r#"
#include "blyt.h"
void blyt_cart_init(void) {}
void blyt_cart_update(void) { blyt_quit(); }
void blyt_cart_draw(void) {}
"#;

/// Run `test_session_api palette <cart> <lib_dir>` and parse its
/// "PALETTE:<256 x 8-hex>" stdout line into 256 XRGB8888 u32s.
fn session_palette(cart: &std::path::Path) -> Vec<u32> {
    let out = Command::new(test_session_api())
        .args([
            "palette",
            cart.to_str().unwrap(),
            sdk_dir().join("lib").to_str().unwrap(),
        ])
        .assert()
        .success()
        .get_output()
        .stdout
        .clone();
    let text = String::from_utf8_lossy(&out);
    let line = text
        .lines()
        .find(|l| l.starts_with("PALETTE:"))
        .unwrap_or_else(|| panic!("no PALETTE: line in output: {text}"));
    let hex = &line["PALETTE:".len()..];
    assert_eq!(hex.len(), 256 * 8, "expected 256 XRGB8888 hex entries");
    (0..256)
        .map(|i| u32::from_str_radix(&hex[i * 8..i * 8 + 8], 16).unwrap())
        .collect()
}

/// A cart with no `palettes:` declaration auto-loads the console default
/// (aurora) before init() — index 0 is black, index 255 is aurora's pinned
/// sacrificial slot #911437.
#[test]
fn undeclared_palette_defaults_to_aurora() {
    require_sdk();
    require_test_session_api();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("pal_default");
    CartProject::new().c(PLAIN_CART_C).write(&project);

    let cart = common::build_cart(&project);
    let pal = session_palette(&cart);
    assert_eq!(pal[0], 0x0000_0000, "aurora index 0 is black");
    assert_eq!(
        pal[255], 0x0091_1437,
        "aurora index 255 is the transparency slot"
    );
}

/// `palettes: default: vga` in blyt.config.yaml auto-loads the VGA built-in
/// instead of aurora — distinguishing entries prove it's the right table
/// (vga index 1 is 0x0000AA, distinct from aurora's grayscale-ramp index 1).
#[test]
fn declared_default_vga_loads_vga() {
    require_sdk();
    require_test_session_api();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("pal_vga");
    CartProject::new()
        .c(PLAIN_CART_C)
        .config("palettes:\n  default: vga\n")
        .write(&project);

    let cart = common::build_cart(&project);
    let pal = session_palette(&cart);
    assert_eq!(pal[0], 0x0000_0000, "vga index 0 is black");
    assert_eq!(
        pal[1], 0x0000_00AA,
        "vga index 1 (blue) distinguishes it from aurora"
    );
    assert_eq!(
        pal[255], 0x0000_0000,
        "vga's unused tail (248-255) is black-padded"
    );
}

/// An unknown `palettes: default:` name is a build error naming the bad value
/// and the allowed set — no silent fallback (ADR-0088's "all silent handling
/// is rejected" principle).
#[test]
fn unknown_palette_name_is_a_build_error() {
    require_sdk();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("pal_bad");
    CartProject::new()
        .c(PLAIN_CART_C)
        .config("palettes:\n  default: palette_vga\n")
        .write(&project);

    let out = common::build_cart_expect_failure(&project);
    assert!(out.contains("unknown built-in palette"), "{out}");
    assert!(out.contains("palette_vga"), "{out}");
}

// ── #199 palette-sensitive oracle ─────────────────────────────────────────
//
// #201 fixed host-Lua palette-set routing for hybrid carts but could not PROVE
// it: `[blyt:fbhash]` hashes palette *indices*, so a frame drawn under the wrong
// palette hashes identically to one drawn under the right palette.  The
// `[blyt:palhash]` oracle (FNV-1a over the active palette bytes, emitted on every
// leg) closes that gap.  These tests assert colour parity — fbhash AND palhash —
// across the emulated trio; the native bare-metal leg is covered by the QEMU gate.

/// Build a C cart whose `blyt_cart_draw` runs `draw_body` once, then quits.
fn build_c_draw_cart(dir: &std::path::Path, draw_body: &str) -> std::path::PathBuf {
    let src = format!(
        "#include \"blyt.h\"\n\
         void blyt_cart_init(void) {{}}\n\
         void blyt_cart_update(void) {{ blyt_quit(); }}\n\
         void blyt_cart_draw(void) {{\n{draw_body}}}\n"
    );
    CartProject::new().c(&src).write(dir);
    build_cart(dir)
}

/// The 256-entry palette a cart declaring `palettes: default: <name>` loads —
/// the same bytes `blyt_gfx_palette_set` loads for that built-in at runtime.
fn declared_palette(dir: &std::path::Path, name: &str) -> Vec<u32> {
    CartProject::new()
        .c(PLAIN_CART_C)
        .config(&format!("palettes:\n  default: {name}\n"))
        .write(dir);
    session_palette(&build_cart(dir))
}

/// A uniform `blyt_gfx_clear(5)` fills every pixel with index 5 — palette-blind,
/// so the index fbhash is identical no matter which palette is active.  Only
/// palhash can tell an aurora frame from a VGA frame: `blyt_gfx_palette_set(VGA)`
/// must therefore change palhash (not fbhash) identically on native, wasm, and
/// libretro.  This is the pure-C dispatch counterpart of the hybrid host-Lua path.
#[test]
fn palette_set_changes_only_palhash_across_legs() {
    require_sdk();
    require_wasm();
    require_libretro_core();
    require_test_session_api();

    let tmp = TempDir::new().unwrap();
    let plain = build_c_draw_cart(&tmp.path().join("plain"), "  blyt_gfx_clear(5);\n");
    let vga = build_c_draw_cart(
        &tmp.path().join("vga"),
        "  blyt_gfx_palette_set(BLYT_PALETTE_VGA);\n  blyt_gfx_clear(5);\n",
    );

    let fb = gfx::expected_hash_line(&gfx::render(&[gfx::Op::Clear(5)]));
    let aurora_ph = gfx::expected_palhash_line(&session_palette(&plain));
    let vga_ph = gfx::expected_palhash_line(&declared_palette(&tmp.path().join("vga_decl"), "vga"));
    assert_ne!(
        aurora_ph, vga_ph,
        "sanity: aurora and vga must have distinct palette hashes"
    );

    let env = [("BLYT_FRAME_HASH", "1")];
    for (cart, ph) in [(&plain, &aurora_ph), (&vga, &vga_ph)] {
        // Same index hash under both palettes — fbhash is palette-blind.
        run_cart_native_with_env(cart, &env, &fb);
        run_cart_wasm_with_env(cart, &env, &fb);
        run_cart_libretro_with_env(cart, &env, &fb);
        // Palette hash tracks the active palette — the oracle #199 needs.
        run_cart_native_with_env(cart, &env, ph);
        run_cart_wasm_with_env(cart, &env, ph);
        run_cart_libretro_with_env(cart, &env, ph);
    }
}

/// The direct #199 regression: a HYBRID cart whose **Lua half** calls
/// `palette_set`.  On wasm the Lua half runs on the host-Lua fast path (no
/// emulator) while the native half runs in rv32emu (ADR-0130); before #201 the
/// host-Lua `palette_set` wrote a parallel buffer, so the presented frame kept
/// the default palette and its colours diverged from the emulated legs.  With the
/// routing fixed, the presented palhash is VGA on every leg — which only the
/// palette-sensitive oracle can confirm (the index fbhash is identical either
/// way).
#[test]
fn hybrid_lua_palette_set_routes_to_session_across_legs() {
    require_sdk();
    require_lua_sdk();
    require_wasm();
    require_libretro_core();
    require_test_session_api();

    let tmp = TempDir::new().unwrap();
    let dir = tmp.path().join("hybrid_pal");
    // A C lib makes the cart session-backed; the Lua half sets the palette (VGA =
    // 0x21000002, the BLYT_PALETTE_VGA handle) and draws a uniform frame.
    CartProject::new()
        .c("#include \"blyt.h\"\nBLYT_LUA_EXPORT_VOID(native_noop) {}\n")
        .lua(
            "function init() end\n\
             function update() blyt.quit() end\n\
             function draw()\n\
             \x20 native_noop()\n\
             \x20 blyt32.gfx.palette_set(0x21000002)\n\
             \x20 blyt32.gfx.clear(5)\n\
             end\n",
        )
        .write(&dir);
    let cart = build_lua_cart(&dir);

    let fb = gfx::expected_hash_line(&gfx::render(&[gfx::Op::Clear(5)]));
    let vga_ph = gfx::expected_palhash_line(&declared_palette(&tmp.path().join("vga_decl"), "vga"));

    let env = [("BLYT_FRAME_HASH", "1")];
    run_cart_native_with_env(&cart, &env, &fb);
    run_cart_wasm_with_env(&cart, &env, &fb);
    run_cart_libretro_with_env(&cart, &env, &fb);
    run_cart_native_with_env(&cart, &env, &vga_ph);
    run_cart_wasm_with_env(&cart, &env, &vga_ph);
    run_cart_libretro_with_env(&cart, &env, &vga_ph);
}

// ── #204 palette-agnostic test card ───────────────────────────────────────

/// The test card carries no palette of its own (#204): it renders its reference
/// colours as the nearest index in the *active* palette.  So an idle cart that
/// declares VGA must render a test card whose expanded colours differ from the
/// aurora-default golden, and — since nearest-index is a pure function — the two
/// host-runtime legs (blytplay and wasm) must produce byte-identical frames.
#[test]
fn testcard_tracks_declared_palette_across_legs() {
    require_sdk();
    require_wasm();

    let golden = std::fs::read(repo_root().join("tests/testcard_frame0.bin"))
        .expect("reading aurora testcard golden");

    let tmp = TempDir::new().unwrap();

    // An idle cart with no declaration renders the test card through the aurora
    // default — this pins the golden to the blytplay leg too (the wasm leg is
    // pinned by wasm_testcard_frame0_matches_golden).
    let idle = tmp.path().join("idle_default");
    CartProject::new().c(PLAIN_CART_C).write(&idle);
    let aurora_frame = dump_frame0_native(&build_cart(&idle));
    assert_eq!(
        aurora_frame, golden,
        "blytplay's default-palette test card must match the aurora golden"
    );

    // An idle cart declaring VGA renders the SAME test card geometry through the
    // VGA palette — different colours, identical across legs.
    let idle_vga = tmp.path().join("idle_vga");
    CartProject::new()
        .c(PLAIN_CART_C)
        .config("palettes:\n  default: vga\n")
        .write(&idle_vga);
    let cart = build_cart(&idle_vga);
    let native = dump_frame0_native(&cart);
    let wasm = dump_frame0_wasm(&cart);

    assert_eq!(native.len(), gfx::FRAME_BYTES, "frame is 320x240 XRGB8888");
    assert_ne!(
        native, golden,
        "the test card must track the declared palette, not carry a fixed one (#204)"
    );
    assert_eq!(
        native, wasm,
        "test card colours must be identical across the blytplay and wasm legs"
    );
}
