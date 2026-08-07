//! `blyt_console_debug` line framing across execution legs (#291).
//!
//! The console-debug API is **line-oriented**: one call emits exactly one line,
//! and the runtime — not the cart — appends the trailing newline (see the
//! declaration in `runtime/guest/include/blyt.h` and the note on ADR-0085).
//!
//! The three host runtimes have always framed `"%s\n"` (the emulated
//! `BLYT_ECALL_CONSOLE_DEBUG` handler's `log_fn`, `libretro_log`, blytplay's
//! `sdl_log`); bare metal issued a raw `write(2)` with nothing appended, so a
//! whole run arrived as one unbroken line. That divergence is what this file
//! pins on the host side; the bare-metal half is Gate 36 in `native_qemu.rs`,
//! which is the leg the fix actually changed.
//!
//! Framing is asserted with `assert_marker_lines_exact`, which compares whole
//! *lines* — unlike the payload-level `cart_markers` scan the other cross-leg
//! tests use, which is deliberately blind to where the line breaks fall and so
//! cannot see this contract at all.

mod common;

use common::{
    assert_marker_lines_exact, build_cart, capture_cart_libretro, capture_cart_native,
    capture_cart_wasm, require_libretro_core, require_sdk, require_wasm, write_c_cart_project,
};
use tempfile::TempDir;

/// The tag the cart wraps every marker in, so the comparison sees only cart
/// output and never a leg's own banner or trace lines.
const M: &str = "lf";

/// A C cart — no Lua anywhere, so every leg runs the emulated rv32 path and the
/// framing under test is the console API's, not a host-Lua driver's.
///
/// The two back-to-back calls in `update()` are the load-bearing case: adjacent
/// calls are exactly what ran together into a single line on the unframed leg.
const CART: &str = r#"
#include "blyt.h"
static int s_frame = 0;
void blyt_cart_init(void)   { blyt_console_debug("<lf:init>"); }
void blyt_cart_update(void) {
    blyt_console_debug("<lf:update>");
    blyt_console_debug("<lf:adjacent>");
    if (++s_frame >= 2) blyt_quit();
}
void blyt_cart_draw(void)   { blyt_console_debug("<lf:draw>"); }
"#;

/// Every console_debug call lands on its own line, identically on all three
/// host-runtime legs. Green before #291 as well as after — the host legs are
/// the *correct* side of that divergence, and this is what pins them there so a
/// future change to `libretro_log`/`sdl_log`/`log_fn` framing cannot silently
/// re-open it from the other end.
#[test]
fn console_debug_frames_one_line_per_call_on_all_host_legs() {
    require_sdk();
    require_wasm();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("console_framing");
    write_c_cart_project(&project, CART);
    let cart = build_cart(&project);

    let expected = [
        "<lf:init>",
        "<lf:update>",
        "<lf:adjacent>",
        "<lf:draw>",
        "<lf:update>",
        "<lf:adjacent>",
        "<lf:draw>",
    ];

    assert_marker_lines_exact("native", &capture_cart_native(&cart, &[]), M, &expected);
    assert_marker_lines_exact("wasm", &capture_cart_wasm(&cart, &[]), M, &expected);
    assert_marker_lines_exact("libretro", &capture_cart_libretro(&cart, &[]), M, &expected);
}
