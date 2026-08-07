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
    run_cart_all_legs_frame_hash_exact, sdk_dir, test_session_api,
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

/// An unknown `palettes: default:` name (neither a built-in nor a palette
/// asset) is a build error naming the bad value and the allowed set — no silent
/// fallback (ADR-0088's "all silent handling is rejected" principle). Since
/// #214 the check moved from config-parse to build-time resolution (a name may
/// be a palette-file asset), so the message is "unknown palette …".
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
    assert!(out.contains("unknown palette"), "{out}");
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

    for (cart, ph) in [(&plain, &aurora_ph), (&vga, &vga_ph)] {
        // Same index hash under both palettes — fbhash is palette-blind.
        run_cart_all_legs_frame_hash_exact(cart, &fb);
        // Palette hash tracks the active palette — the oracle #199 needs.
        run_cart_all_legs_frame_hash_exact(cart, ph);
    }
}

/// The pure-Lua counterpart of [`palette_set_changes_only_palhash_across_legs`]
/// (#231): a Lua cart's `blyt32.gfx.palette_set(blyt32.gfx.PALETTE_VGA)` must
/// change palhash — not the palette-blind fbhash — identically on every leg,
/// including blytplay's native host-Lua fast path, whose runner owns the palette
/// directly (there is no session).  The VGA bytes come from the single built-in
/// resolver, so the palhash matches the emulated legs' declared-VGA golden.
#[test]
fn lua_palette_set_changes_only_palhash_across_legs() {
    require_sdk();
    require_lua_sdk();
    require_wasm();
    require_libretro_core();
    require_test_session_api();

    let tmp = TempDir::new().unwrap();
    let build_lua = |dir: &std::path::Path, draw: &str| -> std::path::PathBuf {
        CartProject::new()
            .lua(&format!(
                "function init() end\n\
                 function update() blyt.quit() end\n\
                 function draw()\n{draw}end\n"
            ))
            .write(dir);
        build_cart(dir)
    };
    let plain = build_lua(&tmp.path().join("lua_plain"), "  blyt32.gfx.clear(5)\n");
    let vga = build_lua(
        &tmp.path().join("lua_vga"),
        "  blyt32.gfx.palette_set(blyt32.gfx.PALETTE_VGA)\n  blyt32.gfx.clear(5)\n",
    );

    let fb = gfx::expected_hash_line(&gfx::render(&[gfx::Op::Clear(5)]));
    let aurora_ph = gfx::expected_palhash_line(&session_palette(&plain));
    let vga_ph =
        gfx::expected_palhash_line(&declared_palette(&tmp.path().join("lua_vga_decl"), "vga"));
    assert_ne!(
        aurora_ph, vga_ph,
        "sanity: aurora and vga must have distinct palette hashes"
    );

    for (cart, ph) in [(&plain, &aurora_ph), (&vga, &vga_ph)] {
        // fbhash is palette-blind — identical on every host-Lua leg.
        run_cart_all_legs_frame_hash_exact(cart, &fb);
        // palhash tracks the active palette on every host-Lua leg.
        run_cart_all_legs_frame_hash_exact(cart, ph);
    }
}

/// The #217 pin: a pure-Lua (session-less) cart that *draws content* via
/// `blyt32.gfx.*` with **no `palettes:` declaration and no `palette_set`**
/// presents through the console default (aurora) — byte-identically on every
/// host-Lua leg (blytplay / wasm-fastpath / libretro).
///
/// Sibling coverage only touches half of this each: [`lua_palette_set_changes_
/// only_palhash_across_legs`] pins the undeclared default's palhash but for a
/// uniform `clear(5)` (palette-blind fbhash), and `gfx.rs`'s torture-frame Lua
/// test pins the drawn fbhash but not palhash. Here a real content frame is drawn
/// under the *undeclared* default so fbhash (the pixels) **and** palhash (the
/// fully-expanded aurora colours) are pinned together — exercising
/// `wasm_main.c`'s `lua_gfx_palette_ensure_default` on the actual drawing path,
/// not just the test-card path that was the only prior exerciser of that default.
///
/// Aurora is pinned to the named built-in (`declared_palette(_, "aurora")`),
/// independent of the cart under test, with a VGA foil so a wrong default palette
/// cannot slip through as a false pass.
#[test]
fn lua_undeclared_default_palette_draw_parity_across_legs() {
    require_sdk();
    require_lua_sdk();
    require_wasm();
    require_libretro_core();
    require_test_session_api();

    let ops = gfx::torture_frame();
    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("lua_default_draw");
    CartProject::new()
        .lua(&format!(
            "function init() end\n\
             function update() blyt.quit() end\n\
             function draw()\n{}end\n",
            gfx::lua_draw_body(&ops)
        ))
        .write(&project);
    let cart = build_cart(&project);

    let fb = gfx::expected_hash_line(&gfx::render(&ops));
    let aurora_ph =
        gfx::expected_palhash_line(&declared_palette(&tmp.path().join("aurora_decl"), "aurora"));
    let vga_ph = gfx::expected_palhash_line(&declared_palette(&tmp.path().join("vga_decl"), "vga"));
    assert_ne!(
        aurora_ph, vga_ph,
        "sanity: aurora and vga must have distinct palette hashes"
    );

    // The undeclared cart draws real content under the aurora default: fbhash
    // pins the drawn pixels, palhash pins the fully-expanded aurora colours —
    // both identical across native / wasm / libretro.
    run_cart_all_legs_frame_hash_exact(&cart, &fb);
    run_cart_all_legs_frame_hash_exact(&cart, &aurora_ph);
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

    run_cart_all_legs_frame_hash_exact(&cart, &fb);
    run_cart_all_legs_frame_hash_exact(&cart, &vga_ph);
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

// ── #214 custom cart palette (.hex/.gpl/.pal) ─────────────────────────────
//
// A cart authors its own 256-colour palette from a palette *file* asset; the
// packer stages it as a PROV_CART resource and emits a typed `R_<NAME>` /
// `R.<NAME>` constant.  It becomes the active palette either as the declared
// `palettes: default:` (pre-init auto-load) or via a runtime `palette_set`.
// These assert identical `[blyt:palhash]` across the emulated trio; the native
// bare-metal leg is the QEMU gate (native_qemu.rs).

/// A distinctive 4-colour custom palette as Lospec `.hex` text, and the 256-entry
/// XRGB8888 table it stages to (colours in 0..3, black-padded) — the bytes the
/// palhash oracle must observe on every leg.
fn custom_palette_hex() -> &'static str {
    "112233\n445566\n778899\naabbcc\n"
}
fn custom_palette_table() -> Vec<u32> {
    let mut t = vec![0u32; 256];
    t[0] = 0x0011_2233;
    t[1] = 0x0044_5566;
    t[2] = 0x0077_8899;
    t[3] = 0x00AA_BBCC;
    t
}

/// `palettes: default: main` naming a `.hex` **asset** (not a built-in)
/// auto-loads the cart-authored palette before init() — proving the PROV_CART
/// branch of the session-create resolver.  The staged bytes reach the session
/// (session_palette), differ from aurora, and every leg's palhash matches.
#[test]
fn custom_default_palette_loads_across_legs() {
    require_sdk();
    require_wasm();
    require_libretro_core();
    require_test_session_api();

    let tmp = TempDir::new().unwrap();
    let dir = tmp.path().join("custom_default");
    CartProject::new()
        .c(&format!(
            "#include \"blyt.h\"\n\
             void blyt_cart_init(void) {{}}\n\
             void blyt_cart_update(void) {{ blyt_quit(); }}\n\
             void blyt_cart_draw(void) {{ blyt_gfx_clear(5); }}\n"
        ))
        .asset("main.hex", custom_palette_hex())
        .config("palettes:\n  default: main\n")
        .write(&dir);
    let cart = build_cart(&dir);

    // The build+load path delivers the custom bytes to the session palette.
    assert_eq!(
        session_palette(&cart),
        custom_palette_table(),
        "the .hex asset must auto-load as the session palette"
    );

    let custom_ph = gfx::expected_palhash_line(&custom_palette_table());
    let aurora_ph =
        gfx::expected_palhash_line(&declared_palette(&tmp.path().join("aurora_decl"), "aurora"));
    assert_ne!(
        custom_ph, aurora_ph,
        "sanity: the custom palette must differ from aurora"
    );

    let fb = gfx::expected_hash_line(&gfx::render(&[gfx::Op::Clear(5)]));
    run_cart_all_legs_frame_hash_exact(&cart, &fb);
    run_cart_all_legs_frame_hash_exact(&cart, &custom_ph);
}

/// A runtime `blyt_gfx_palette_set(R_MAIN)` on a C cart whose default is aurora
/// switches to the cart-authored palette — proving the PROV_CART branch of the
/// GFX_PALETTE_SET ECALL resolver.  The switched-to palhash is the custom
/// palette on every leg.
#[test]
fn runtime_palette_set_custom_across_legs() {
    require_sdk();
    require_wasm();
    require_libretro_core();
    require_test_session_api();

    let tmp = TempDir::new().unwrap();
    let dir = tmp.path().join("custom_runtime");
    CartProject::new()
        .c(&format!(
            "#include \"blyt.h\"\n\
             #include \"cart_resources.h\"\n\
             void blyt_cart_init(void) {{}}\n\
             void blyt_cart_update(void) {{ blyt_quit(); }}\n\
             void blyt_cart_draw(void) {{ blyt_gfx_palette_set(R_MAIN); blyt_gfx_clear(5); }}\n"
        ))
        .asset("main.hex", custom_palette_hex())
        .write(&dir);
    let cart = build_cart(&dir);

    let custom_ph = gfx::expected_palhash_line(&custom_palette_table());
    let fb = gfx::expected_hash_line(&gfx::render(&[gfx::Op::Clear(5)]));
    run_cart_all_legs_frame_hash_exact(&cart, &fb);
    run_cart_all_legs_frame_hash_exact(&cart, &custom_ph);
}

/// The pure-Lua counterpart: a Lua cart passes the typed palette constant
/// `R.MAIN` to `blyt32.gfx.palette_set`.  On wasm this runs on the host-Lua fast
/// path (g_session == NULL), so it exercises `lua_resolve_palette` against
/// `g_lua_resources` — the fast-path mirror of the emulated resolver.  Same
/// custom palhash on native (emulated Lua), wasm (fast path), and libretro.
#[test]
fn lua_custom_palette_set_across_legs() {
    require_sdk();
    require_lua_sdk();
    require_wasm();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let dir = tmp.path().join("custom_lua");
    CartProject::new()
        .lua(
            "local R = require(\"cart_resources\")\n\
             function init() end\n\
             function update() blyt.quit() end\n\
             function draw()\n\
             \x20 blyt32.gfx.palette_set(R.MAIN)\n\
             \x20 blyt32.gfx.clear(5)\n\
             end\n",
        )
        .asset("main.hex", custom_palette_hex())
        .write(&dir);
    let cart = build_lua_cart(&dir);

    let custom_ph = gfx::expected_palhash_line(&custom_palette_table());
    let fb = gfx::expected_hash_line(&gfx::render(&[gfx::Op::Clear(5)]));
    run_cart_all_legs_frame_hash_exact(&cart, &fb);
    run_cart_all_legs_frame_hash_exact(&cart, &custom_ph);
    // resource table (no session) → hl_resolve_palette → the custom palhash.
}

// ── #219 pure-Lua declared-default palette on the WASM host-Lua fast path ──
//
// A pure-Lua cart that DECLARES `palettes: default:` and never calls
// `palette_set` must render through that palette from frame 0 on EVERY leg.  On
// wasm a session-less pure-Lua cart runs on the host-Lua fast path, which seeds
// its own `g_lua_gfx_palette`; before #219 that seed was hardcoded to aurora and
// ignored the declared default — a cross-runtime divergence (the fast path
// diverged from native/libretro, which auto-load the declared palette at
// session-create).  These assert the declared palette's `[blyt:palhash]` is
// identical across native/wasm/libretro with NO explicit `palette_set` — so they
// exercise exactly the auto-load path #214's tests sidestepped.

/// A pure-Lua cart declaring a **built-in** default (`vga`) — no `palette_set` —
/// must present VGA's palhash on every leg, the fast path included.  Red on the
/// wasm leg before #219 (fast path showed aurora).
#[test]
fn lua_declared_builtin_default_across_legs() {
    require_sdk();
    require_lua_sdk();
    require_wasm();
    require_libretro_core();
    require_test_session_api();

    let tmp = TempDir::new().unwrap();
    let dir = tmp.path().join("lua_default_vga");
    CartProject::new()
        .lua(
            "function init() end\n\
             function update() blyt.quit() end\n\
             function draw() blyt32.gfx.clear(5) end\n",
        )
        .config("palettes:\n  default: vga\n")
        .write(&dir);
    let cart = build_lua_cart(&dir);

    let vga_ph = gfx::expected_palhash_line(&declared_palette(&tmp.path().join("vga_decl"), "vga"));
    let fb = gfx::expected_hash_line(&gfx::render(&[gfx::Op::Clear(5)]));
    run_cart_all_legs_frame_hash_exact(&cart, &fb);
    run_cart_all_legs_frame_hash_exact(&cart, &vga_ph);
    // runner's palette (hl_palette_ensure_default), no palette_set.
}

/// The custom-palette counterpart: a pure-Lua cart declaring a `.hex` **asset**
/// as its default — no `palette_set` — must present the cart-authored palhash on
/// every leg.  Exercises the fast path resolving a PROV_CART default at load
/// time (the resource table must be populated first).
#[test]
fn lua_declared_custom_default_across_legs() {
    require_sdk();
    require_lua_sdk();
    require_wasm();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let dir = tmp.path().join("lua_default_custom");
    CartProject::new()
        .lua(
            "function init() end\n\
             function update() blyt.quit() end\n\
             function draw() blyt32.gfx.clear(5) end\n",
        )
        .asset("main.hex", custom_palette_hex())
        .config("palettes:\n  default: main\n")
        .write(&dir);
    let cart = build_lua_cart(&dir);

    let custom_ph = gfx::expected_palhash_line(&custom_palette_table());
    let fb = gfx::expected_hash_line(&gfx::render(&[gfx::Op::Clear(5)]));
    run_cart_all_legs_frame_hash_exact(&cart, &fb);
    run_cart_all_legs_frame_hash_exact(&cart, &custom_ph);
    // load time against the runner's resource table (hl_palette_ensure_default).
}

// ── #221 remaining custom-palette parity cells (hybrid + custom on the fast
// path) ────────────────────────────────────────────────────────────────────
//
// A HYBRID cart resolves a custom palette against the SESSION resource table on
// the wasm host-Lua fast path (`active_resource_table()` ->
// `blyt_session_resources(g_session)`) — a branch #214's hybrid test never hit
// (it used a built-in handle, which skips the CART-resource lookup).  These
// close that cell for both entry points.

/// #221 cell 1: a hybrid cart whose Lua half calls `palette_set(R.MAIN)` with a
/// **custom** asset.  Exercises the session-resource-table branch of
/// `lua_resolve_palette` on wasm.  Custom palhash identical on every leg.
#[test]
fn hybrid_custom_palette_set_across_legs() {
    require_sdk();
    require_lua_sdk();
    require_wasm();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let dir = tmp.path().join("hybrid_custom_set");
    CartProject::new()
        .c("#include \"blyt.h\"\nBLYT_LUA_EXPORT_VOID(native_noop) {}\n")
        .lua(
            "local R = require(\"cart_resources\")\n\
             function init() end\n\
             function update() blyt.quit() end\n\
             function draw()\n\
             \x20 native_noop()\n\
             \x20 blyt32.gfx.palette_set(R.MAIN)\n\
             \x20 blyt32.gfx.clear(5)\n\
             end\n",
        )
        .asset("main.hex", custom_palette_hex())
        .write(&dir);
    let cart = build_lua_cart(&dir);

    let custom_ph = gfx::expected_palhash_line(&custom_palette_table());
    let fb = gfx::expected_hash_line(&gfx::render(&[gfx::Op::Clear(5)]));
    run_cart_all_legs_frame_hash_exact(&cart, &fb);
    run_cart_all_legs_frame_hash_exact(&cart, &custom_ph);
}

/// #221 cell 2: a hybrid cart declaring a **custom** `.hex` default and NO
/// `palette_set`.  `cart_run.c` auto-loads it into the session palette at
/// session-create; on wasm the Lua half reads that session palette.  Custom
/// palhash identical on every leg.
#[test]
fn hybrid_custom_default_across_legs() {
    require_sdk();
    require_lua_sdk();
    require_wasm();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let dir = tmp.path().join("hybrid_custom_default");
    CartProject::new()
        .c("#include \"blyt.h\"\nBLYT_LUA_EXPORT_VOID(native_noop) {}\n")
        .lua(
            "function init() end\n\
             function update() blyt.quit() end\n\
             function draw()\n\
             \x20 native_noop()\n\
             \x20 blyt32.gfx.clear(5)\n\
             end\n",
        )
        .asset("main.hex", custom_palette_hex())
        .config("palettes:\n  default: main\n")
        .write(&dir);
    let cart = build_lua_cart(&dir);

    let custom_ph = gfx::expected_palhash_line(&custom_palette_table());
    let fb = gfx::expected_hash_line(&gfx::render(&[gfx::Op::Clear(5)]));
    run_cart_all_legs_frame_hash_exact(&cart, &fb);
    run_cart_all_legs_frame_hash_exact(&cart, &custom_ph);
}
