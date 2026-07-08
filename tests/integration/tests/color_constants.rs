//! Named color-index constants (issue #203, ADR-0059) — cross-language,
//! cross-leg parity.
//!
//! `[blyt:fbhash]` hashes palette *indices*, not RGB, so a cart that draws
//! using the SHIPPED named constants (`BLYT_EGA_*` / `BLYT_VGA_*` /
//! `BLYT_AURORA_*` / the unprefixed default aliases) produces a frame hash
//! determined entirely by the index each constant names. Drawing the identical
//! picture from C, Lua, and Rust and asserting one golden across all three
//! languages × all three emulated legs therefore proves the constants resolve
//! to the SAME index everywhere: a per-language typo (e.g. Rust's
//! `AURORA_RED` off by one, or the wasm host-Lua table drifting from `blyt.h`)
//! diverges from the golden and fails here. The golden is computed by
//! `common::gfx::render` from the explicit indices, while the draw bodies use
//! the names — the pairing is the cross-check.
//!
//! `test_color_constants` (a host C unit test) separately pins that each index
//! is the intended *color* in the palette bytes; this suite pins that the SDK
//! constants agree across languages and every runtime leg.

mod common;

use common::gfx;
use common::{
    CartProject, build_cart, require_lua_sdk, require_rust_riscv_target, require_sdk,
    run_cart_libretro_with_env, run_cart_native_hostlua_frame_hash, run_cart_native_with_env,
    run_cart_wasm_with_env, write_c_cart_project,
};

/// One horizontal band of the shared test picture: filled with palette index
/// `idx`, named by the matching constant in each language. The `Rect` op uses
/// `idx`; the draw bodies use `c`/`lua`/`rust` — so if any name resolves to a
/// different index, that language's frame diverges from the golden.
struct Band {
    y: i32,
    h: i32,
    idx: u8,
    c: &'static str,
    lua: &'static str,
    rust: &'static str,
}

/// A spread across all three palette sets plus a default alias, so a typo in
/// any one set is caught. Indices are the canonical EGA (0-15) / Aurora
/// (#203 table) values, mirrored by `blyt.h` / `blyt::color` / `blyt32.color`.
fn bands() -> Vec<Band> {
    vec![
        // Aurora chromatic (hue-shifted nearest-to-EGA).
        Band {
            y: 0,
            h: 48,
            idx: 155,
            c: "BLYT_AURORA_RED",
            lua: "blyt32.color.aurora.RED",
            rust: "blyt::color::aurora::RED",
        },
        // EGA canonical low-16.
        Band {
            y: 48,
            h: 48,
            idx: 2,
            c: "BLYT_EGA_GREEN",
            lua: "blyt32.color.ega.GREEN",
            rust: "blyt::color::ega::GREEN",
        },
        // VGA alias of the EGA 16.
        Band {
            y: 96,
            h: 48,
            idx: 11,
            c: "BLYT_VGA_BR_CYAN",
            lua: "blyt32.color.vga.BR_CYAN",
            rust: "blyt::color::vga::BR_CYAN",
        },
        // Unprefixed default alias → Aurora.
        Band {
            y: 144,
            h: 48,
            idx: 175,
            c: "BLYT_BR_YELLOW",
            lua: "blyt32.color.BR_YELLOW",
            rust: "blyt::color::BR_YELLOW",
        },
        // Aurora exact gray.
        Band {
            y: 192,
            h: 48,
            idx: 10,
            c: "BLYT_AURORA_LTGRAY",
            lua: "blyt32.color.aurora.LTGRAY",
            rust: "blyt::color::aurora::LTGRAY",
        },
    ]
}

/// Ops for the golden: clear to black (index 0, `*_BLACK` in every set), then
/// one full-width rect per band at its explicit index.
fn picture_ops(bands: &[Band]) -> Vec<gfx::Op> {
    let mut ops = vec![gfx::Op::Clear(0)];
    for b in bands {
        ops.push(gfx::Op::Rect(0, b.y, gfx::FRAME_W as i32, b.h, b.idx));
    }
    ops
}

/// C `blyt_cart_draw` body using the named constants.
fn c_body(bands: &[Band]) -> String {
    let mut s = "  blyt_gfx_clear(BLYT_AURORA_BLACK);\n".to_string();
    for b in bands {
        s += &format!(
            "  blyt_gfx_rect_fill(0, {}, {}, {}, {});\n",
            b.y,
            gfx::FRAME_W,
            b.h,
            b.c
        );
    }
    s
}

/// Lua `draw()` body using the named constants.
fn lua_body(bands: &[Band]) -> String {
    let mut s = "  blyt32.gfx.clear(blyt32.color.aurora.BLACK)\n".to_string();
    for b in bands {
        s += &format!(
            "  blyt32.gfx.rect_fill(0, {}, {}, {}, {})\n",
            b.y,
            gfx::FRAME_W,
            b.h,
            b.lua
        );
    }
    s
}

/// Rust `blyt_cart_draw` body using the named constants.
fn rust_body(bands: &[Band]) -> String {
    let mut s = "    blyt::gfx::SCREEN.clear(blyt::color::aurora::BLACK);\n".to_string();
    for b in bands {
        s += &format!(
            "    blyt::gfx::SCREEN.rect_fill(0, {}, {}, {}, {});\n",
            b.y,
            gfx::FRAME_W,
            b.h,
            b.rust
        );
    }
    s
}

fn build_c_cart(dir: &std::path::Path, draw_body: &str) -> std::path::PathBuf {
    let src = format!(
        "#include \"blyt.h\"\n\
         void blyt_cart_init(void) {{}}\n\
         void blyt_cart_update(void) {{ blyt_quit(); }}\n\
         void blyt_cart_draw(void) {{\n{draw_body}}}\n"
    );
    write_c_cart_project(dir, &src);
    build_cart(dir)
}

fn build_lua_draw_cart(dir: &std::path::Path, draw_body: &str) -> std::path::PathBuf {
    CartProject::new()
        .lua(&format!(
            "function init() end\n\
             function update() blyt.quit() end\n\
             function draw()\n{draw_body}end\n"
        ))
        .write(dir);
    build_cart(dir)
}

fn build_rust_draw_cart(dir: &std::path::Path, draw_body: &str) -> std::path::PathBuf {
    let src = format!(
        "#![no_std]\n\
         #[no_mangle]\n\
         pub extern \"C\" fn blyt_cart_init() {{}}\n\
         #[no_mangle]\n\
         pub extern \"C\" fn blyt_cart_update() {{ blyt::quit(); }}\n\
         #[no_mangle]\n\
         pub extern \"C\" fn blyt_cart_draw() {{\n{draw_body}}}\n"
    );
    CartProject::new().rust(&src).write(dir);
    build_cart(dir)
}

/// The shipped `BLYT_EGA_*` / `BLYT_VGA_*` / `BLYT_AURORA_*` sets (+ default
/// alias) draw the same picture, hashing to one golden across C / Lua / Rust
/// and every emulated leg — proving the constants agree everywhere. No per-cart
/// index definitions are used (AC: "text / basic-draw works with a bundled
/// palette using only shipped constants").
#[test]
fn named_color_sets_hash_identically_across_langs_and_legs() {
    require_sdk();
    let bands = bands();
    let expected = gfx::expected_hash_line(&gfx::render(&picture_ops(&bands)));
    let env = [("BLYT_FRAME_HASH", "1")];
    let tmp = tempfile::tempdir().unwrap();

    // C.
    let c_cart = build_c_cart(&tmp.path().join("colors-c"), &c_body(&bands));
    run_cart_native_with_env(&c_cart, &env, &expected);
    run_cart_wasm_with_env(&c_cart, &env, &expected);
    run_cart_libretro_with_env(&c_cart, &env, &expected);

    // Lua (emulated-Lua on native/libretro, host-Lua fast path on wasm).
    require_lua_sdk();
    let lua_cart = build_lua_draw_cart(&tmp.path().join("colors-lua"), &lua_body(&bands));
    run_cart_native_with_env(&lua_cart, &env, &expected);
    run_cart_wasm_with_env(&lua_cart, &env, &expected);
    run_cart_libretro_with_env(&lua_cart, &env, &expected);
    run_cart_native_hostlua_frame_hash(&lua_cart, &expected); // #231 (blyt32.color)

    // Rust.
    require_rust_riscv_target();
    let rust_cart = build_rust_draw_cart(&tmp.path().join("colors-rust"), &rust_body(&bands));
    run_cart_native_with_env(&rust_cart, &env, &expected);
    run_cart_wasm_with_env(&rust_cart, &env, &expected);
    run_cart_libretro_with_env(&rust_cart, &env, &expected);
}

/// Packer-generated `C_<NAME>` constants (from a manifest `colors:` swatch map)
/// resolve to the declared index in C / Lua / Rust and every leg. The cart
/// clears the screen to `C_BRAND` (declared index 200); the frame must hash to a
/// uniform fill of 200 everywhere — proving the generated constant carries the
/// right value through each language's build path.
#[test]
fn packer_generated_color_constants_hash_identically_across_langs_and_legs() {
    require_sdk();
    const BRAND: u8 = 200;
    let config = format!("colors:\n  brand: {BRAND}\n");
    let expected = gfx::expected_hash_line(&gfx::render(&[gfx::Op::Clear(BRAND)]));
    let env = [("BLYT_FRAME_HASH", "1")];
    let tmp = tempfile::tempdir().unwrap();

    // C: #include the generated header, use C_BRAND.
    let c_dir = tmp.path().join("cnames-c");
    CartProject::new()
        .config(&config)
        .c("#include \"blyt.h\"\n\
            #include \"cart_colors.h\"\n\
            void blyt_cart_init(void) {}\n\
            void blyt_cart_update(void) { blyt_quit(); }\n\
            void blyt_cart_draw(void) { blyt_gfx_clear(C_BRAND); }\n")
        .write(&c_dir);
    let c_cart = build_cart(&c_dir);
    run_cart_native_with_env(&c_cart, &env, &expected);
    run_cart_wasm_with_env(&c_cart, &env, &expected);
    run_cart_libretro_with_env(&c_cart, &env, &expected);

    // Lua: require the generated module, use C.BRAND.
    require_lua_sdk();
    let lua_dir = tmp.path().join("cnames-lua");
    CartProject::new()
        .config(&config)
        .lua(
            "local C = require(\"cart_colors\")\n\
              function init() end\n\
              function update() blyt.quit() end\n\
              function draw() blyt32.gfx.clear(C.BRAND) end\n",
        )
        .write(&lua_dir);
    let lua_cart = build_cart(&lua_dir);
    run_cart_native_with_env(&lua_cart, &env, &expected);
    run_cart_wasm_with_env(&lua_cart, &env, &expected);
    run_cart_libretro_with_env(&lua_cart, &env, &expected);

    // Rust: include! the generated module, use C_BRAND.
    require_rust_riscv_target();
    let rust_dir = tmp.path().join("cnames-rust");
    CartProject::new()
        .config(&config)
        .rust(
            "#![no_std]\n\
               include!(env!(\"BLYT_CART_COLORS_RS\"));\n\
               #[no_mangle]\n\
               pub extern \"C\" fn blyt_cart_init() {}\n\
               #[no_mangle]\n\
               pub extern \"C\" fn blyt_cart_update() { blyt::quit(); }\n\
               #[no_mangle]\n\
               pub extern \"C\" fn blyt_cart_draw() { blyt::gfx::SCREEN.clear(C_BRAND); }\n",
        )
        .write(&rust_dir);
    let rust_cart = build_cart(&rust_dir);
    run_cart_native_with_env(&rust_cart, &env, &expected);
    run_cart_wasm_with_env(&rust_cart, &env, &expected);
    run_cart_libretro_with_env(&rust_cart, &env, &expected);
}
