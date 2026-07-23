//! Cross-leg capture-helper diagnostics (#295).
//!
//! The `capture_cart_*` helpers in `common` return a cart's output for a caller
//! to parse a value out of (heap bytes, `mem.stats` fields, frame hashes). When a
//! cart *aborts* — a Lua error, an S-proxy type failure — the runtime prints the
//! diagnostic to **stderr** and the cart exits before writing the value to stdout.
//! A stdout-only capture then returns an empty (or value-less) string and the
//! caller panics with a generic "no such line" that reads as a *harness* bug,
//! discarding the real `[blyt] ...:N: <message>` cause. That cost real debugging
//! time on #270 (the `set_u32 = 4e9` i32-overflow trap).
//!
//! These helpers must therefore surface stderr so an aborted leg is diagnosed as
//! the cart/runtime error it is. This is the capture-side sibling of #284's
//! assert-side gap (substring assertions hiding extra/repeated output).

mod common;

use common::{
    CartProject, build_lua_cart, capture_cart_libretro, capture_cart_native, capture_cart_wasm,
    require_libretro_core, require_lua_sdk, require_sdk, require_wasm,
};
use tempfile::TempDir;

/// A pure-Lua cart that raises a Lua error in `init()` **before** writing
/// anything to stdout, so a stdout-only capture sees nothing and the underlying
/// message is visible only if the helper surfaces stderr.
const ABORT_IN_INIT: &str = r#"
function init()
  error("boom295-abort-marker")
end
function update() blyt.quit() end
function draw() end
"#;

/// Every cross-leg capture helper must surface an aborting cart's error text, so
/// the failure is diagnosed as a cart/runtime abort rather than a bare missing
/// line. Before the #295 fix `capture_cart_wasm` returned stdout only, dropping
/// the `[blyt] ...:N: boom295` line the WASM runtime writes to stderr.
#[test]
fn aborting_cart_error_is_surfaced_on_every_leg() {
    require_sdk();
    require_lua_sdk();
    require_wasm();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("abort_in_init");
    CartProject::new().lua(ABORT_IN_INIT).write(&project);
    let cart = build_lua_cart(&project);

    let needle = "boom295-abort-marker";
    for (leg, out) in [
        ("native", capture_cart_native(&cart, &[])),
        ("wasm", capture_cart_wasm(&cart, &[])),
        ("libretro", capture_cart_libretro(&cart, &[])),
    ] {
        assert!(
            out.contains(needle),
            "the {leg} capture helper must surface the aborting cart's error \
             ({needle:?}); the runtime prints it to stderr, so a stdout-only \
             capture drops it and the cause reads as a harness bug (#295). \
             captured output was:\n{out}"
        );
    }
}
