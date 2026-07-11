//! Hybrid carts on the native host-Lua fast path (#232, epic #230, ADR-0136 +
//! ADR-0130).
//!
//! A hybrid cart (Lua + a native C/Rust half) routed through blytplay's native
//! host-Lua VM runs the Lua half on the host `lua_State` while the native half
//! stays EMULATED under rv32emu, bridged via the ADR-0130 ECALL Lua C API — the
//! native counterpart of the WASM host-Lua hybrid path (`wasm_main.c`
//! run_lua_cart). Determinism across every leg is the core contract (ADR-0007),
//! so each test asserts the SAME cart-visible output on all five legs:
//!   * the emulated leg        (`run_cart_native` — blytplay, rv32emu),
//!   * the native host-Lua leg (`run_cart_native_with_env` + `BLYT_HOSTLUA=1`),
//!   * the WASM host-Lua leg   (`run_cart_wasm`),
//!   * the libretro emulated `.so` leg (`run_cart_libretro`), and
//!   * the libretro host-Lua `.so` leg (`run_cart_libretro_with_env` +
//!     `BLYT_HOSTLUA=1`) — the embedded core's host-Lua path with the ADR-0130
//!     bridge stub embedded (#232 S5).
//! A divergence between the native host-Lua bridge and the reference fails here
//! by construction rather than relying on per-leg smoke (anti-#98).
//!
//! Slices land incrementally: S1 typed export, S2 raw/bridged export, S3 state
//! buffers + save/restore + reset-every-frame, S4 native-lifecycle, S5 libretro
//! + C++ + Rust bridged + the actual hello-lua-c / hello-lua-rust examples.

mod common;

use common::{
    CartProject, build_lua_cart, require_cpp_sdk, require_libretro_core, require_lua_sdk,
    require_rust_riscv_target, require_sdk, require_wasm, run_cart_libretro,
    run_cart_libretro_with_env, run_cart_libretro_with_env_and_flags, run_cart_libretro_with_flags,
    run_cart_native, run_cart_native_with_env, run_cart_wasm, run_cart_wasm_with_env,
};
use std::path::Path;
use tempfile::TempDir;

/// Run blytplay --headless with both extra env (e.g. BLYT_HOSTLUA=1) and flags
/// (e.g. --reset-every-frame); assert `expected` in stdout. (Mirrors hostlua.rs.)
fn run_native_env_flags(
    cart: &std::path::Path,
    env: &[(&str, &str)],
    flags: &[&str],
    expected: &str,
) {
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
    assert!(
        s.contains(expected),
        "expected {expected:?} (env={env:?} flags={flags:?}): {s}"
    );
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
    require_libretro_core();

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
    // libretro core — emulated + host-Lua (embedded bridge stub, #232 S5).
    run_cart_libretro(&cart, expected);
    run_cart_libretro_with_env(&cart, &[("BLYT_HOSTLUA", "1")], expected);
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
    require_libretro_core();

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
    run_cart_libretro(&cart, expected);
    run_cart_libretro_with_env(&cart, &[("BLYT_HOSTLUA", "1")], expected);
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
    require_libretro_core();

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

    // Plain run — every execution model agrees.
    run_cart_native(&cart, expected);
    run_cart_native_with_env(&cart, &[("BLYT_HOSTLUA", "1")], expected);
    run_cart_wasm(&cart, expected);
    run_cart_libretro(&cart, expected);
    run_cart_libretro_with_env(&cart, &[("BLYT_HOSTLUA", "1")], expected);

    // Reset-every-frame — the host-Lua VM-rebuild cycle must reach the SAME
    // trajectory as the emulated (BSS-zero) and WASM legs, with the state buffers
    // shared with the persisting rv32 session across each rebuild.
    run_native_env_flags(&cart, &[], &["--reset-every-frame"], expected);
    run_native_env_flags(
        &cart,
        &[("BLYT_HOSTLUA", "1")],
        &["--reset-every-frame"],
        expected,
    );
    run_cart_wasm_with_env(&cart, &[("BLYT_RESET_EVERY_FRAME", "1")], expected);
    run_cart_libretro_with_flags(&cart, &["--reset-every-frame"], expected);
    run_cart_libretro_with_env_and_flags(
        &cart,
        &[("BLYT_HOSTLUA", "1")],
        &["--reset-every-frame"],
        expected,
    );
}

/// S4 — a native-lifecycle hybrid: the C half defines `blyt_cart_update`,
/// overriding libblyt32lua's Lua-driver stub, so the runtime drives the native
/// update each frame while `init`/`draw` stay in Lua. On the native host-Lua leg
/// the native lifecycle fn is injected as a zero-arg Lua-global trampoline so the
/// runner's `call_lifecycle` drives it through the ADR-0130 bridge (the native
/// counterpart of wasm_main.c's `maybe_inject_lifecycle_cb`), and the native
/// half's `blyt_quit()` during update must propagate to end the loop identically
/// on every leg. A `--quit-after` safety cap on the native legs turns a broken
/// bridge (native update never runs → cart never quits) into a clean assertion
/// failure rather than a hang.
#[test]
fn hostlua_hybrid_native_lifecycle_parity() {
    require_sdk();
    require_lua_sdk();
    require_wasm();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("hostlua_hybrid_native_lifecycle");

    CartProject::new()
        .c(r#"#include "blyt.h"
/* Native lifecycle: this cart's own blyt_cart_update overrides libblyt32lua's
 * Lua-driver stub, so the runtime calls it natively each frame while init()/draw()
 * stay in Lua. blyt_quit() from the native half must end the loop on every leg. */
static int32_t ticks = 0;
void blyt_cart_update(void) {
    ticks++;
    if (ticks == 5) {
        blyt_console_debug("native update reached tick 5");
        blyt_quit();
    }
}
"#)
        .lua(
            r#"
function init()
    blyt.debug.print("lua init before native update loop")
end
function draw() end
"#,
        )
        .write(&project);

    let cart = build_lua_cart(&project);
    let expected = "native update reached tick 5";

    // Emulated (rv32emu) — the reference; native blyt_cart_update drives directly.
    run_native_env_flags(&cart, &[], &["--quit-after", "60"], expected);
    // Native host-Lua fast path (#232 S4) — native lifecycle injected as a global.
    run_native_env_flags(
        &cart,
        &[("BLYT_HOSTLUA", "1")],
        &["--quit-after", "60"],
        expected,
    );
    // WASM host-Lua fast path — the existing native-lifecycle hybrid leg.
    run_cart_wasm(&cart, expected);
    // libretro core — emulated + host-Lua. The cart self-quits at tick 5, so no
    // frame cap is needed; --run-frames guards against a broken host-Lua bridge.
    run_cart_libretro_with_flags(&cart, &["--run-frames", "60"], expected);
    run_cart_libretro_with_env_and_flags(
        &cart,
        &[("BLYT_HOSTLUA", "1")],
        &["--run-frames", "60"],
        expected,
    );
}

/// S5 — a C++ typed-export hybrid: `BLYT_LUA_EXPORT_I32` from `extern "C"` over a
/// C++ helper. Proves the host-Lua bridge is language-agnostic across the native
/// half (C in S1, C++ here) — the same typed marshalling path, five-leg identical.
#[test]
fn hostlua_hybrid_cpp_typed_export_parity() {
    require_sdk();
    require_cpp_sdk();
    require_lua_sdk();
    require_wasm();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("hostlua_hybrid_cpp");

    CartProject::new()
        .cpp(
            r#"#include "blyt.h"
static int cpp_cube(int x) { return x * x * x; }
extern "C" {
BLYT_LUA_EXPORT_I32(cube, int32_t x) { return (int32_t)cpp_cube((int)x); }
}
"#,
        )
        .lua(
            r#"
function init()
    blyt.debug.print("lua+cpp cube=" .. cube(3))
end
function update() blyt.quit() end
function draw() end
"#,
        )
        .write(&project);

    let cart = build_lua_cart(&project);
    let expected = "lua+cpp cube=27";

    run_cart_native(&cart, expected);
    run_cart_native_with_env(&cart, &[("BLYT_HOSTLUA", "1")], expected);
    run_cart_wasm(&cart, expected);
    run_cart_libretro(&cart, expected);
    run_cart_libretro_with_env(&cart, &[("BLYT_HOSTLUA", "1")], expected);
}

/// S5 — a Rust raw/bridged export hybrid: the exact `#[lua_export(module =
/// "greeting", raw)]` shape hello-lua-rust ships, exercising the Rust bridge
/// codegen (ADR-0130) end-to-end through the host-Lua exchange thread, five-leg
/// identical. Complements the C bridged export (S2) so both native languages'
/// raw-export macros are covered on the native host-Lua path.
#[test]
fn hostlua_hybrid_rust_bridged_export_parity() {
    require_sdk();
    require_lua_sdk();
    require_rust_riscv_target();
    require_wasm();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("hostlua_hybrid_rust_bridged");

    CartProject::new()
        .rust(
            r#"#![no_std]
extern crate blyt;
use blyt::lua::{api, lua_export, LuaState};

#[lua_export(module = "greeting", raw)]
fn log(l: LuaState) {
    let (s, _len) = api::check_lstring(l, 1);
    blyt::console_debug_ptr(s);
}
"#,
        )
        .lua(
            r#"
local greeting = require("greeting")
function init()
    greeting.log("lua+rust bridged log line")
end
function update() blyt.quit() end
function draw() end
"#,
        )
        .write(&project);

    let cart = build_lua_cart(&project);
    let expected = "lua+rust bridged log line";

    run_cart_native(&cart, expected);
    run_cart_native_with_env(&cart, &[("BLYT_HOSTLUA", "1")], expected);
    run_cart_wasm(&cart, expected);
    run_cart_libretro(&cart, expected);
    run_cart_libretro_with_env(&cart, &[("BLYT_HOSTLUA", "1")], expected);
}

/// Copy the tree at `src` into `dst`, skipping any generated `build/` dir, so an
/// in-repo example can be built in a scratch dir without polluting the checkout
/// or racing a parallel test that builds the same example.
fn copy_tree(src: &Path, dst: &Path) {
    std::fs::create_dir_all(dst).unwrap();
    for entry in std::fs::read_dir(src).unwrap() {
        let entry = entry.unwrap();
        if entry.file_name() == "build" {
            continue;
        }
        let from = entry.path();
        let to = dst.join(entry.file_name());
        if from.is_dir() {
            copy_tree(&from, &to);
        } else {
            std::fs::copy(&from, &to).unwrap();
        }
    }
}

/// Build the verbatim in-repo example `example` into a scratch dir, appending a
/// bounded quit to its `main.lua` so the (otherwise never-quitting) example
/// terminates cleanly on every leg. Only a terminator is added — its
/// update/draw/greeting logic and native half stay verbatim. `main.lua` is a
/// single chunk, so the appended wrapper closes over the same `frame`/`update`.
fn build_example_capped(example: &str, tmp: &Path) -> std::path::PathBuf {
    let src = common::repo_root().join("examples").join(example);
    let dst = tmp.join(example);
    copy_tree(&src, &dst);
    let main_lua = dst.join("src/game/lua/main.lua");
    let mut body = std::fs::read_to_string(&main_lua).unwrap();
    body.push_str(
        "\nlocal __blyt_orig_update = update\n\
         function update()\n\
         \x20   __blyt_orig_update()\n\
         \x20   if frame >= 20 then blyt.quit() end\n\
         end\n",
    );
    std::fs::write(&main_lua, body).unwrap();
    build_lua_cart(&dst)
}

/// S5 acceptance — the actual `examples/hello-lua-c` (Lua + a C `greeting.log`
/// raw export + state buffers) runs on the native host-Lua path with output
/// identical to the emulated + WASM + libretro legs (the PLAN.md acceptance
/// criterion, using the real example sources rather than a synthetic clone).
#[test]
fn hostlua_hybrid_example_hello_lua_c_parity() {
    require_sdk();
    require_lua_sdk();
    require_wasm();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let cart = build_example_capped("hello-lua-c", tmp.path());
    let expected = "update frame 20 player pos: 162, 122";

    run_cart_native(&cart, expected);
    run_cart_native_with_env(&cart, &[("BLYT_HOSTLUA", "1")], expected);
    run_cart_wasm(&cart, expected);
    run_cart_libretro(&cart, expected);
    run_cart_libretro_with_env(&cart, &[("BLYT_HOSTLUA", "1")], expected);
}

/// S5 acceptance — the actual `examples/hello-lua-rust` (Lua + a Rust
/// `#[lua_export(module = "greeting", raw)]` half) runs on the native host-Lua
/// path with output identical across every leg. This is the real Rust bridge
/// example, the second half of PLAN.md's acceptance criterion.
#[test]
fn hostlua_hybrid_example_hello_lua_rust_parity() {
    require_sdk();
    require_lua_sdk();
    require_rust_riscv_target();
    require_wasm();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let cart = build_example_capped("hello-lua-rust", tmp.path());
    let expected = "update frame 20 player pos: 162, 122";

    run_cart_native(&cart, expected);
    run_cart_native_with_env(&cart, &[("BLYT_HOSTLUA", "1")], expected);
    run_cart_wasm(&cart, expected);
    run_cart_libretro(&cart, expected);
    run_cart_libretro_with_env(&cart, &[("BLYT_HOSTLUA", "1")], expected);
}
