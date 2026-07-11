//! Hybrid carts on the native host-Lua fast path (#232, epic #230, ADR-0136 +
//! ADR-0130).
//!
//! A hybrid cart (Lua + a native C/Rust half) routed through blytplay's native
//! host-Lua VM runs the Lua half on the host `lua_State` while the native half
//! stays EMULATED under rv32emu, bridged via the ADR-0130 ECALL Lua C API — the
//! native counterpart of the WASM host-Lua hybrid path (`wasm_main.c`
//! run_lua_cart). Determinism across every leg is the core contract (ADR-0007),
//! so each test asserts the SAME cart-visible output on:
//!   * the emulated leg      (`run_cart_native` — blytplay, rv32emu),
//!   * the native host-Lua leg (`run_cart_native_with_env` + `BLYT_HOSTLUA=1`),
//!   * the WASM host-Lua leg (`run_cart_wasm`),
//! and, once the libretro leg lands (S5), the libretro emulated + host-Lua `.so`
//! legs. A divergence between the native host-Lua bridge and the reference fails
//! here by construction rather than relying on per-leg smoke (anti-#98).
//!
//! Slices land incrementally: S1 typed export, S2 raw/bridged export, S3 state
//! buffers + save/restore + reset-every-frame, S4 native-lifecycle, S5 libretro
//! + C++ + actual examples.

mod common;

use common::{
    build_lua_cart, require_lua_sdk, require_sdk, require_wasm, run_cart_native,
    run_cart_native_with_env, run_cart_wasm, run_cart_wasm_with_env, CartProject,
};
use tempfile::TempDir;

/// Run blytplay --headless with both extra env (e.g. BLYT_HOSTLUA=1) and flags
/// (e.g. --reset-every-frame); assert `expected` in stdout. (Mirrors hostlua.rs.)
fn run_native_env_flags(cart: &std::path::Path, env: &[(&str, &str)], flags: &[&str], expected: &str) {
    use assert_cmd::Command;
    let mut cmd = Command::new(common::blytplay());
    cmd.arg("--headless");
    for f in flags {
        cmd.arg(f);
    }
    cmd.arg(cart.to_str().unwrap());
    for (k, v) in env {
        cmd.env(k, v);
    }
    let out = cmd.assert().success().get_output().stdout.clone();
    let s = String::from_utf8_lossy(&out);
    assert!(s.contains(expected), "expected {expected:?} (env={env:?} flags={flags:?}): {s}");
}

/// S1 — a typed export (`BLYT_LUA_EXPORT_I32`, ≤4 scalar args, no bridge ECALLs):
/// the host-Lua VM marshals the Lua args into rv32 registers, drives the emulated
/// native function to completion, and reads back its single scalar return. The
/// native host-Lua leg must produce the same line as the emulated + WASM legs.
#[test]
fn hostlua_hybrid_typed_export_parity() {
    require_sdk();
    require_lua_sdk();
    require_wasm();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("hostlua_hybrid_typed");

    CartProject::new()
        .c("#include \"blyt.h\"\n\
             BLYT_LUA_EXPORT_I32(add_one, int32_t x) { return x + 1; }\n")
        .lua(
            r#"
function init()
    local r = add_one(41)
    blyt.debug.print("lua+c typed add_one=" .. r)
end
function update() blyt.quit() end
function draw() end
"#,
        )
        .write(&project);

    let cart = build_lua_cart(&project);
    let expected = "lua+c typed add_one=42";

    // Emulated (rv32emu) — the reference.
    run_cart_native(&cart, expected);
    // Native host-Lua fast path (#232) — the leg under test.
    run_cart_native_with_env(&cart, &[("BLYT_HOSTLUA", "1")], expected);
    // WASM host-Lua fast path — the existing hybrid host-Lua leg.
    run_cart_wasm(&cart, expected);
}

/// S2 — a raw/bridged export (`BLYT_LUA_MODULE_EXPORT_RAW`, ADR-0130): the guest
/// wrapper reads its own Lua args (a string here) and pushes its own results
/// through the ECALL bridge against the exchange thread. This is the acceptance
/// shape (`hello-lua-c`'s `greeting.log`). Exercises string marshalling and the
/// module.fn dotted-name registration + `require()`.
#[test]
fn hostlua_hybrid_bridged_export_parity() {
    require_sdk();
    require_lua_sdk();
    require_wasm();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("hostlua_hybrid_bridged");

    CartProject::new()
        .c(r#"#include "blyt.h"
/* Acceptance shape (hello-lua-c): a raw export that reads a string arg and
 * writes it to the console from the emulated native half. */
BLYT_LUA_MODULE_EXPORT_RAW(greeting, log) {
    const char *s = luaL_checkstring(L, 1);
    blyt_console_debug(s);
    return 0;
}
/* Also exercise a bridged string RETURN back to Lua (PUSHLSTRING path). */
BLYT_LUA_MODULE_EXPORT_RAW(greeting, echo) {
    const char *s = luaL_checkstring(L, 1);
    lua_pushstring(L, s);
    return 1;
}
"#)
        .lua(
            r#"
local greeting = require("greeting")
function init()
    greeting.log("hybrid-bridge-log")
    blyt.debug.print("echo:" .. greeting.echo("hybrid-bridge-echo"))
end
function update() blyt.quit() end
function draw() end
"#,
        )
        .write(&project);

    let cart = build_lua_cart(&project);
    // Both legs emit the native console line then the Lua echo line; assert the
    // echo (which round-trips a string through the bridge in both directions).
    let expected = "echo:hybrid-bridge-echo";

    run_cart_native(&cart, expected);
    run_cart_native_with_env(&cart, &[("BLYT_HOSTLUA", "1")], expected);
    run_cart_wasm(&cart, expected);
}

/// S3 — the hello-lua-c acceptance surface: state buffers (S proxy) + entity ref
/// + a plain Lua `frame` serialised through on_save_state/on_load_state, with the
/// output line emitted by a native raw export (`greeting.log`). This makes it a
/// hybrid whose state ctx is the rv32 session's (shared with the emulated native
/// half), exercised across every leg AND under --reset-every-frame. The native
/// host-Lua leg must reach the SAME trajectory as the emulated + WASM legs and be
/// invariant to reset-every-frame (the VM-rebuild save/clear/restore cycle).
#[test]
fn hostlua_hybrid_state_buffers_save_restore_parity() {
    require_sdk();
    require_lua_sdk();
    require_wasm();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("hostlua_hybrid_state");

    CartProject::new()
        .c(r#"#include "blyt.h"
BLYT_LUA_MODULE_EXPORT_RAW(greeting, log) {
    const char *s = luaL_checkstring(L, 1);
    blyt_console_debug(s);
    return 0;
}
"#)
        .config(
            "\
records:
  Globals:
    fields:
      - { name: frame, type: i32 }
      - { name: player, ref: character }
  Character:
    fields:
      - { name: x, type: i32 }
      - { name: y, type: i32 }
state_buffers:
  globals:
    record: Globals
    count: 1
  character:
    record: Character
    count: 1
",
        )
        .lua(
            r#"
local greeting = require("greeting")
local frame

function init()
    frame = 0
end

function on_new_state()
    blyt.buf.alloc_slot(S.GLOBALS)
    local slot = blyt.buf.alloc_slot(S.CHARACTER)
    S.globals[0].player = blyt.buf.ref(S.CHARACTER, slot)
    S.character[slot].x = 160
    S.character[slot].y = 120
    greeting.log("init player pos: 160, 120")
end

function update()
    frame = frame + 1
    if frame % 10 == 0 then
        local player = S.globals[0].player
        if blyt.buf.ref_valid(S.CHARACTER, player) then
            local slot = blyt.buf.ref_slot(player)
            local x = (S.character[slot].x + 1) % 320
            local y = (S.character[slot].y + 1) % 240
            S.character[slot].x = x
            S.character[slot].y = y
            greeting.log("update frame " .. frame .. " player pos: " .. x .. ", " .. y)
        end
    end
    if frame >= 20 then blyt.quit() end
end

function draw() end

function on_save_state()
    S.globals[0].frame = frame
end

function on_load_state(_info)
    frame = S.globals[0].frame
end
"#,
        )
        .write(&project);

    let cart = build_lua_cart(&project);
    let expected = "update frame 20 player pos: 162, 122";

    // Plain run — the three execution models agree.
    run_cart_native(&cart, expected);
    run_cart_native_with_env(&cart, &[("BLYT_HOSTLUA", "1")], expected);
    run_cart_wasm(&cart, expected);

    // Reset-every-frame — the host-Lua VM-rebuild cycle must reach the SAME
    // trajectory as the emulated (BSS-zero) and WASM legs, with the state buffers
    // shared with the persisting rv32 session across each rebuild.
    run_native_env_flags(&cart, &[], &["--reset-every-frame"], expected);
    run_native_env_flags(&cart, &[("BLYT_HOSTLUA", "1")], &["--reset-every-frame"], expected);
    run_cart_wasm_with_env(&cart, &[("BLYT_RESET_EVERY_FRAME", "1")], expected);
}
