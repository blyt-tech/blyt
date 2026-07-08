//! Native host-Lua fast path parity (#238, epic #230, ADR-0136).
//!
//! A pure-Lua cart routed through blytplay's native host-Lua VM (opt-in
//! `--host-lua` / `BLYT_HOSTLUA=1`) must produce the SAME cart-visible output as
//! the emulated RV32 Lua VM and the WASM host-Lua fast path — determinism across
//! every leg is the core contract (ADR-0007). These tests assert the same
//! `expected` line on:
//!   * the emulated leg   (`run_cart_native` — blytplay, rv32emu),
//!   * the host-Lua leg   (`run_cart_native_with_env` + `BLYT_HOSTLUA=1`),
//!   * the WASM leg        (`run_cart_wasm` — host-Lua fast path on wasm32),
//! so a divergence between the native host-Lua VM and the reference fails here by
//! construction rather than relying on per-leg smoke.
//!
//! S2 scope: the minimal surface a pure-Lua cart reaches for output and
//! termination — `blyt.debug.print` in `init()` and `blyt.quit()`. State buffers
//! (S3), full `hello` save/restore (S4) and the libretro `.so` under opt-in (S5)
//! extend this file as those slices land.

mod common;

use common::{
    CartProject, build_lua_cart, require_lua_sdk, require_sdk, require_wasm, run_cart_native,
    run_cart_native_with_env, run_cart_wasm,
};
use tempfile::TempDir;

/// A minimal pure-Lua cart: print one line in `init()`, then quit. The native
/// host-Lua VM must emit the same line as the emulated and WASM legs.
#[test]
fn hostlua_debug_print_parity() {
    require_sdk();
    require_lua_sdk();
    require_wasm();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("hostlua_hello");

    CartProject::new()
        .lua(
            r#"
function init()
    blyt.debug.print("hello from host-lua")
end

function update()
    blyt.quit()
end

function draw() end
"#,
        )
        .write(&project);

    let cart = build_lua_cart(&project);
    let expected = "hello from host-lua";

    // Emulated (rv32emu) — the reference.
    run_cart_native(&cart, expected);
    // Native host-Lua fast path (#238) — the new leg under test.
    run_cart_native_with_env(&cart, &[("BLYT_HOSTLUA", "1")], expected);
    // WASM host-Lua fast path — the existing native-VM leg.
    run_cart_wasm(&cart, expected);
}

/// `blyt.quit()` called from `init()` terminates before any `update()`/`draw()`
/// runs — a marker printed in `init()` appears exactly once, and one printed in
/// `update()` must NOT appear, on the host-Lua leg just as on the emulated leg.
#[test]
fn hostlua_quit_in_init_skips_update() {
    require_sdk();
    require_lua_sdk();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("hostlua_quit_init");

    CartProject::new()
        .lua(
            r#"
function init()
    blyt.debug.print("init-ran")
    blyt.quit()
end

function update()
    blyt.debug.print("update-ran")
end

function draw() end
"#,
        )
        .write(&project);

    let cart = build_lua_cart(&project);

    for env in [&[][..], &[("BLYT_HOSTLUA", "1")][..]] {
        use assert_cmd::Command;
        let mut cmd = Command::new(common::blytplay());
        cmd.args(["--headless", "--quit-after", "5", cart.to_str().unwrap()]);
        for (k, v) in env {
            cmd.env(k, v);
        }
        let out = cmd.assert().success().get_output().stdout.clone();
        let s = String::from_utf8_lossy(&out);
        assert!(
            s.contains("init-ran"),
            "init did not run (env={env:?}): {s}"
        );
        assert!(
            !s.contains("update-ran"),
            "update ran despite quit() in init (env={env:?}): {s}"
        );
    }
}
