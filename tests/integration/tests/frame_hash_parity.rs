//! Teeth for the exact frame-hash parity helper (#284).
//!
//! `run_cart_all_legs_frame_hash` asserts only that the expected hash line
//! *appears* in each leg's output, so a leg that diverges on a non-target frame,
//! renders a different number of frames, or repeats one still passes — the
//! #280/#283 determinism-hole class. `run_cart_all_legs_frame_hash_exact`
//! compares the full ordered `[blyt:fbhash]`/`[blyt:palhash]` sequence across
//! legs instead.
//!
//! Determinism holds today, so there is no real cross-leg divergence to ablate
//! against; the unit tests below prove the comparison rejects divergence by
//! feeding it synthetic sequences, and the integration test proves a real
//! multi-frame cart produces one identical sequence on every leg.

mod common;

use common::{
    build_cart, frame_hash_lines, frame_hash_parity_error, gfx, require_libretro_core, require_sdk,
    require_wasm, run_cart_all_legs_frame_hash_exact, write_c_cart_project,
};

fn seq(lines: &[&str]) -> Vec<String> {
    lines.iter().map(|s| s.to_string()).collect()
}

const FB0: &str = "[blyt:fbhash] 0000000000000000";
const FB1: &str = "[blyt:fbhash] 1111111111111111";
const PAL: &str = "[blyt:palhash] aaaaaaaaaaaaaaaa";

/// Identical multi-line sequences containing the target hash → agreement.
#[test]
fn parity_holds_when_all_legs_match_and_expected_present() {
    let s = seq(&[FB0, PAL, FB1, PAL]);
    assert_eq!(frame_hash_parity_error(&s, &s, &s, FB1), None);
    // The palette hash is equally a valid pin target.
    assert_eq!(frame_hash_parity_error(&s, &s, &s, PAL), None);
}

/// A leg that diverges on a *non-target* frame passes the substring check but
/// must fail the exact one — the core #280/#283 tooth.
#[test]
fn parity_fails_when_a_leg_diverges_on_a_non_target_frame() {
    let native = seq(&[FB0, PAL, FB1, PAL]);
    // wasm renders a different frame 0 but the same target frame 1: `.contains`
    // on FB1 would pass; the sequence compare must not.
    let wasm = seq(&["[blyt:fbhash] deadbeefdeadbeef", PAL, FB1, PAL]);
    let msg = frame_hash_parity_error(&native, &wasm, &native, FB1)
        .expect("divergent non-target frame must be rejected");
    assert!(
        msg.contains("native and wasm"),
        "message names the legs: {msg}"
    );
}

/// A leg that renders a *different frame count* must be rejected even when the
/// target hash is present on all of them.
#[test]
fn parity_fails_on_frame_count_mismatch() {
    let short = seq(&[FB0, PAL]);
    let long = seq(&[FB0, PAL, FB0, PAL]);
    assert!(frame_hash_parity_error(&short, &short, &long, FB0).is_some());
}

/// All legs agree but on the *wrong* content (target absent) → still rejected,
/// so cross-leg agreement alone can't green a broken frame.
#[test]
fn parity_fails_when_expected_absent_though_legs_agree() {
    let s = seq(&[FB0, PAL]);
    let msg = frame_hash_parity_error(&s, &s, &s, FB1).expect("absent target must be rejected");
    assert!(
        msg.contains("not found"),
        "message flags the missing pin: {msg}"
    );
}

/// Extraction keeps order and strips any leg-specific log prefix, so lines from
/// differently-framed leg logs compare equal.
#[test]
fn frame_hash_lines_strips_prefix_and_preserves_order() {
    let raw = "banner line\n\
               [blyt:fbhash] 00\n\
               [libretro] [blyt:palhash] aa\n\
               unrelated\n\
               prefix: [blyt:fbhash] 11\n";
    assert_eq!(
        frame_hash_lines(raw),
        seq(&["[blyt:fbhash] 00", "[blyt:palhash] aa", "[blyt:fbhash] 11"]),
    );
}

/// A real two-frame C cart draws distinct content each frame (clear to colour 1,
/// then colour 2), so its frame-hash sequence is genuinely multi-valued. Every
/// leg must emit that same sequence — the exact helper proves it on real output,
/// not just synthetic input.
#[test]
fn two_frame_cart_frame_hash_sequence_is_identical_across_legs() {
    require_sdk();
    require_wasm();
    require_libretro_core();

    let src = r#"#include "blyt.h"
static int g_n = 0;
void blyt_cart_init(void) {}
void blyt_cart_update(void) {
  g_n++;
  if (g_n >= 2) blyt_quit();
}
void blyt_cart_draw(void) {
  blyt_surface_clear(BLYT_SCREEN, (unsigned char)(g_n + 1));
}
"#;
    let tmp = tempfile::tempdir().unwrap();
    let dir = tmp.path().join("fh-two-frame");
    write_c_cart_project(&dir, src);
    let cart = build_cart(&dir);

    // Pin frame 1's content (screen cleared to colour 2); the helper additionally
    // proves frame 0 (colour 1) agrees across legs even though it is not pinned.
    let expected = gfx::expected_hash_line(&vec![2u8; gfx::FRAME_W * gfx::FRAME_H]);
    run_cart_all_legs_frame_hash_exact(&cart, &expected);
}
