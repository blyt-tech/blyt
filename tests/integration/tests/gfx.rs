//! Spike X — host-side 2D rasterizer: framebuffer mechanism + determinism.
//!
//! This suite proves the two load-bearing assumptions before the ~50-entry
//! graphics surface is built (issue #188):
//!   Q1 — a single `acquire`/`present` + ECALL-primitive contract spans the
//!        emulated, host-Lua, and native execution models, and
//!   Q2 — one integer rasterizer source hashes bit-identically across every
//!        compile target.
//!
//! The first slice covers the lowest-risk path the brief orders first: the
//! `blyt_gfx_clear` ECALL primitive on the emulated leg, which must fill the
//! host framebuffer and set `cart_has_drawn` so the PM5544 test card is
//! displaced.

mod common;

use common::{
    blytplay, build_cart, require_sdk, run_cart_libretro_with_env, run_cart_native_with_env,
    run_cart_wasm_with_env, write_c_cart_project,
};

const FRAME_W: usize = 320;
const FRAME_H: usize = 240;
const FRAME_BYTES: usize = FRAME_W * FRAME_H * 4; // XRGB8888

// -------------------------------------------------------------------------
// Reference rasterizer (the golden spec, in Rust)
//
// A single op list drives BOTH the generated C cart (which calls the runtime's
// integer rasterizer over the ECALL boundary) AND this reference reimplementation.
// The cart's emitted framebuffer hash must equal the reference's hash, so the
// test catches a wrong rasterizer rather than merely self-confirming a captured
// value, and is palette-independent (it operates on palette indices).  The
// reference mirrors runtime/shared/blyt_raster.c primitive-for-primitive.
// -------------------------------------------------------------------------

#[derive(Clone, Copy)]
enum Op {
    Clear(u8),
    Pixel(i32, i32, u8),
    Rect(i32, i32, i32, i32, u8),
    Line(i32, i32, i32, i32, u8),
}

fn c_draw_body(ops: &[Op]) -> String {
    let mut s = String::new();
    for op in ops {
        match *op {
            Op::Clear(c) => s += &format!("  blyt_gfx_clear({c});\n"),
            Op::Pixel(x, y, c) => s += &format!("  blyt_gfx_pixel({x}, {y}, {c});\n"),
            Op::Rect(x, y, w, h, c) => {
                s += &format!("  blyt_gfx_rect_fill({x}, {y}, {w}, {h}, {c});\n")
            }
            Op::Line(x0, y0, x1, y1, c) => {
                s += &format!("  blyt_gfx_line({x0}, {y0}, {x1}, {y1}, {c});\n")
            }
        }
    }
    s
}

fn put(fb: &mut [u8], x: i32, y: i32, c: u8) {
    if x >= 0 && (x as usize) < FRAME_W && y >= 0 && (y as usize) < FRAME_H {
        fb[y as usize * FRAME_W + x as usize] = c;
    }
}

/// Render the op list to a paletted framebuffer, mirroring blyt_raster.c.
fn render(ops: &[Op]) -> Vec<u8> {
    let mut fb = vec![0u8; FRAME_W * FRAME_H];
    for op in ops {
        match *op {
            Op::Clear(c) => fb.iter_mut().for_each(|p| *p = c),
            Op::Pixel(x, y, c) => put(&mut fb, x, y, c),
            Op::Rect(x, y, w, h, c) => {
                if w > 0 && h > 0 {
                    let x0 = x.max(0) as i64;
                    let y0 = y.max(0) as i64;
                    let x1 = (x as i64 + w as i64).min(FRAME_W as i64);
                    let y1 = (y as i64 + h as i64).min(FRAME_H as i64);
                    for yy in y0..y1 {
                        for xx in x0..x1 {
                            put(&mut fb, xx as i32, yy as i32, c);
                        }
                    }
                }
            }
            Op::Line(mut x0, mut y0, x1, y1, c) => {
                let dx = (x1 - x0).abs();
                let dy = (y1 - y0).abs();
                let sx = if x0 < x1 { 1 } else { -1 };
                let sy = if y0 < y1 { 1 } else { -1 };
                let mut err = dx - dy;
                loop {
                    put(&mut fb, x0, y0, c);
                    if x0 == x1 && y0 == y1 {
                        break;
                    }
                    let e2 = 2 * err;
                    if e2 > -dy {
                        err -= dy;
                        x0 += sx;
                    }
                    if e2 < dx {
                        err += dx;
                        y0 += sy;
                    }
                }
            }
        }
    }
    fb
}

/// The deterministic per-pixel pattern the acquire/present probe writes directly
/// into the runtime-reserved framebuffer region.  A function of (x, y) chosen so
/// every byte differs from its neighbours (catches stride / byte-order bugs in
/// the present copy).  Mirrored exactly by the C cart loop in
/// [`build_raw_present_cart`].
fn raw_pattern_frame() -> Vec<u8> {
    let mut fb = vec![0u8; FRAME_W * FRAME_H];
    for y in 0..FRAME_H {
        for x in 0..FRAME_W {
            fb[y * FRAME_W + x] = (x as i32 * 31 + y as i32 * 17) as u8;
        }
    }
    fb
}

/// FNV-1a 64, matching runtime/shared/blyt_frame_hash.c.
fn fnv1a(bytes: &[u8]) -> u64 {
    let mut h = 0xcbf29ce484222325u64;
    for &b in bytes {
        h ^= b as u64;
        h = h.wrapping_mul(0x100000001b3);
    }
    h
}

/// A frame exercising every primitive plus edge cases the brief calls out:
/// off-screen clipping, zero size, negative coords, and i32-overflowing extents.
fn torture_frame() -> Vec<Op> {
    vec![
        Op::Clear(3),
        Op::Rect(10, 20, 40, 30, 5),    // wholly on-screen
        Op::Rect(-20, -10, 50, 40, 7),  // negative origin, top-left clip
        Op::Rect(300, 220, 40, 40, 9),  // bottom-right clip
        Op::Rect(400, 400, 10, 10, 11), // wholly off-screen -> no-op
        Op::Rect(50, 50, 0, 10, 13),    // zero width -> no-op
        Op::Rect(2_000_000_000, 0, 2_000_000_000, 10, 200), // x+w overflows i32 -> clipped no-op
        Op::Pixel(0, 0, 1),             // corner
        Op::Pixel(319, 239, 2),         // opposite corner
        Op::Pixel(-5, 5, 4),            // off-screen -> no-op
        Op::Pixel(320, 0, 6),           // just off the right edge -> no-op
        Op::Line(0, 0, 319, 239, 8),    // full diagonal
        Op::Line(319, 0, 0, 239, 10),   // anti-diagonal
        Op::Line(-50, 120, 400, 120, 12), // horizontal, off-screen endpoints
        Op::Line(160, -30, 160, 300, 14), // vertical, off-screen endpoints
        Op::Line(5, 5, 5, 5, 15),       // degenerate single point
    ]
}

/// Build a C cart that acquires the raw framebuffer pointer, writes the
/// [`raw_pattern_frame`] pattern straight into that runtime-reserved guest
/// region, presents it, then quits.  Exercises the Q1 acquire/present contract:
/// a guest-VA pointer the cart writes directly (no per-pixel ECALL), flushed to
/// the host framebuffer by one present call.
fn build_raw_present_cart(dir: &std::path::Path) -> std::path::PathBuf {
    let src = format!(
        r#"#include "blyt.h"
void blyt_cart_init(void) {{}}
void blyt_cart_update(void) {{ blyt_quit(); }}
void blyt_cart_draw(void) {{
  unsigned char *fb = blyt_gfx_acquire();
  for (int y = 0; y < {FRAME_H}; y++)
    for (int x = 0; x < {FRAME_W}; x++)
      fb[y * {FRAME_W} + x] = (unsigned char)(x * 31 + y * 17);
  blyt_gfx_present();
}}
"#
    );
    write_c_cart_project(dir, &src);
    build_cart(dir)
}

/// Build a C cart that draws `ops` once then quits.
fn build_drawing_cart(dir: &std::path::Path, ops: &[Op]) -> std::path::PathBuf {
    let src = format!(
        r#"#include "blyt.h"
void blyt_cart_init(void) {{}}
void blyt_cart_update(void) {{ blyt_quit(); }}
void blyt_cart_draw(void) {{
{}}}
"#,
        c_draw_body(ops)
    );
    write_c_cart_project(dir, &src);
    build_cart(dir)
}

/// Dump frame 0 (XRGB8888) of a cart via `blytplay --headless --dump-frame0`.
fn dump_frame0(cart: &std::path::Path) -> Vec<u8> {
    use assert_cmd::Command;
    let tmp = tempfile::NamedTempFile::new().unwrap();
    let path = tmp.path().to_str().unwrap().to_string();
    Command::new(blytplay())
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

    // Probe cart: clear the screen to a fixed palette index every frame.
    let probe = tmp.path().join("gfx-clear");
    write_c_cart_project(
        &probe,
        r#"#include "blyt.h"
void blyt_cart_init(void) {}
void blyt_cart_update(void) {}
void blyt_cart_draw(void) { blyt_gfx_clear(7); }
"#,
    );
    let probe_frame = dump_frame0(&build_cart(&probe));

    // Baseline: a cart that never draws — frame 0 is the PM5544 test card.
    let idle = tmp.path().join("gfx-idle");
    write_c_cart_project(
        &idle,
        r#"#include "blyt.h"
void blyt_cart_init(void) {}
void blyt_cart_update(void) {}
void blyt_cart_draw(void) {}
"#,
    );
    let testcard_frame = dump_frame0(&build_cart(&idle));

    assert_eq!(probe_frame.len(), FRAME_BYTES, "frame is 320x240 XRGB8888");
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
/// (The native QEMU leg — the fourth target — lands with the native libblyt32
/// variant in a later slice; this is the emulated-trio portion of Q2.)
#[test]
fn gfx_torture_frame_hashes_identically_across_legs() {
    require_sdk();
    let ops = torture_frame();

    let tmp = tempfile::tempdir().unwrap();
    let cart = build_drawing_cart(&tmp.path().join("gfx-torture"), &ops);

    let expected = format!("[blyt:fbhash] {:016x}", fnv1a(&render(&ops)));
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
    let cart = build_raw_present_cart(&tmp.path().join("gfx-raw-present"));

    let expected = format!("[blyt:fbhash] {:016x}", fnv1a(&raw_pattern_frame()));
    let env = [("BLYT_FRAME_HASH", "1")];
    run_cart_native_with_env(&cart, &env, &expected);
    run_cart_wasm_with_env(&cart, &env, &expected);
    run_cart_libretro_with_env(&cart, &env, &expected);
}
