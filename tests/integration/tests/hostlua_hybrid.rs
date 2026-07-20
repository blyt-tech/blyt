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
    CartProject, build_lua_cart, capture_cart_libretro, capture_cart_native, capture_cart_wasm,
    require_cpp_sdk, require_libretro_core, require_lua_sdk, require_rust_riscv_target,
    require_sdk, require_wasm, run_cart_libretro, run_cart_libretro_with_env,
    run_cart_libretro_with_env_and_flags, run_cart_libretro_with_flags, run_cart_native,
    run_cart_native_with_env, run_cart_wasm, run_cart_wasm_with_env,
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

/// #261 — the teeth for the native-half BSS reset gap. A hybrid whose NATIVE half
/// parks mutable state in its OWN BSS (a plain module-level counter, deliberately
/// NOT a state buffer) must reset in lockstep with the Lua half under
/// `--reset-every-frame`, identically on every leg.
///
/// The emulated reference (`blyt_reset_every_frame_cycle`) zeroes ALL guest BSS —
/// native half included — and re-runs init() each cycle, so the native counter is
/// back to its post-init value (0) before every frame's update. The host-Lua legs
/// (native runner `blyt_hostlua_reset_every_frame_cycle` and WASM `wasm_lua_rebuild`)
/// rebuild only the Lua VM and round-trip the shared state buffers — the persisting
/// rv32 session's native BSS is untouched — so before the fix the native counter
/// DRIFTS (accumulates) on those legs while it stays reset on the emulated legs: a
/// cross-leg determinism divergence (the #98 silent-half-operation class).
///
/// `frame` is persisted in a state buffer (so the loop advances across reset
/// cycles, like the S3 cart), but the native tick counter is NOT — it is the
/// drift-prone native-BSS state under test. `native_tick()` returns the counter
/// AFTER incrementing, so:
///   * plain run              — no reset, counter accumulates → frame 5 sees 5;
///   * --reset-every-frame     — correct legs zero the native BSS each cycle, so
///                               each frame's single tick returns 1 → frame 5 sees 1.
/// Before the fix the host-Lua legs report 5 under reset-every-frame (accumulated);
/// after it, 1 — matching the emulated legs.
#[test]
fn hostlua_hybrid_native_bss_reset_every_frame_parity() {
    require_sdk();
    require_lua_sdk();
    require_wasm();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("hostlua_hybrid_native_bss");

    CartProject::new()
        .c(r#"#include "blyt.h"
/* Native-half state parked in the cart's OWN BSS, NOT a state buffer: a plain
 * module-level counter. Under --reset-every-frame the emulated leg zeroes guest
 * BSS each cycle, so the host-Lua legs must reset it in lockstep too (#261). */
static int32_t g_native_ticks = 0;
BLYT_LUA_EXPORT_I32(native_tick, int32_t unused) {
    (void)unused;
    g_native_ticks++;
    return g_native_ticks;
}
"#)
        .config(
            "\
records:
  Globals:
    fields:
      - { name: frame, type: i32 }
state_buffers:
  globals:
    record: Globals
    count: 1
",
        )
        .lua(
            r#"
local frame

function init()
    frame = 0
end

function on_new_state()
    blyt.buf.alloc_slot(S.GLOBALS)
end

function update()
    frame = frame + 1
    -- Increment the native-BSS counter every frame; report it at frame 5. The
    -- value distinguishes a leg that resets the native half each cycle (1) from
    -- one that lets its BSS drift (5).
    local ticks = native_tick(0)
    if frame >= 5 then
        blyt.debug.print("native ticks at frame 5 = " .. ticks)
        blyt.quit()
    end
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

    // Plain run — no reset, the native counter accumulates to the frame count on
    // every leg (this is the same for emulated and host-Lua: nothing is reset).
    let expected_plain = "native ticks at frame 5 = 5";
    run_cart_native(&cart, expected_plain);
    run_cart_native_with_env(&cart, &[("BLYT_HOSTLUA", "1")], expected_plain);
    run_cart_wasm(&cart, expected_plain);
    run_cart_libretro(&cart, expected_plain);
    run_cart_libretro_with_env(&cart, &[("BLYT_HOSTLUA", "1")], expected_plain);

    // Reset-every-frame — the native half's BSS must be zeroed each cycle in
    // lockstep with the Lua VM rebuild, so the single per-frame tick returns 1 on
    // EVERY leg. Before the #261 fix the host-Lua legs (native + WASM) would report
    // 5 here (drift), diverging from the emulated legs.
    let expected_reset = "native ticks at frame 5 = 1";
    run_native_env_flags(&cart, &[], &["--reset-every-frame"], expected_reset);
    run_native_env_flags(
        &cart,
        &[("BLYT_HOSTLUA", "1")],
        &["--reset-every-frame"],
        expected_reset,
    );
    run_cart_wasm_with_env(&cart, &[("BLYT_RESET_EVERY_FRAME", "1")], expected_reset);
    run_cart_libretro_with_flags(&cart, &["--reset-every-frame"], expected_reset);
    run_cart_libretro_with_env_and_flags(
        &cart,
        &[("BLYT_HOSTLUA", "1")],
        &["--reset-every-frame"],
        expected_reset,
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

/// S6 — reverse-trampoline (#262): the native half calls a Lua function BACK
/// through the ADR-0130 bridge (`lua_getglobal` + `lua_pcall`), the deferred
/// "reverse-trampoline" ADR-0130 anticipated. A raw export `host.tick()` invokes
/// the Lua global `on_tick(7)` and returns its result, so the observable line
/// (`cb:14`) proves the native→Lua call round-tripped through the exchange
/// thread. Fails on every host-Lua leg until the CALL opcode exists (today the
/// bridge stub has no `lua_pcall`); the emulated + libretro legs link the real
/// in-machine `libblyt32lua`.
#[test]
fn hostlua_hybrid_native_to_lua_callback_parity() {
    require_sdk();
    require_lua_sdk();
    require_wasm();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("hostlua_hybrid_reverse_tramp");

    CartProject::new()
        .c(r#"#include "blyt.h"
/* Reverse-trampoline (#262): call a Lua global back through the bridge and
 * return its result. */
BLYT_LUA_MODULE_EXPORT_RAW(host, tick) {
    lua_getglobal(L, "on_tick"); /* push the Lua callback */
    lua_pushinteger(L, 7);       /* arg */
    lua_pcall(L, 1, 1, 0);       /* on_tick(7) -> single result on the stack */
    return 1;                    /* hand that result back to the Lua caller */
}
"#)
        .lua(
            r#"
local host = require("host")
function on_tick(n)
    return n * 2
end
function init()
    blyt.debug.print("cb:" .. host.tick())
end
function update() blyt.quit() end
function draw() end
"#,
        )
        .write(&project);

    let cart = build_lua_cart(&project);
    let expected = "cb:14";

    run_cart_native(&cart, expected);
    run_cart_native_with_env(&cart, &[("BLYT_HOSTLUA", "1")], expected);
    run_cart_wasm(&cart, expected);
    run_cart_libretro(&cart, expected);
    run_cart_libretro_with_env(&cart, &[("BLYT_HOSTLUA", "1")], expected);
}

/// S6 — reverse-trampoline error semantics (#262): `lua_pcall` returns a status
/// the native half reads (a callee error is CAUGHT, status non-OK), and
/// `lua_call` RE-RAISES the callee's error into the calling Lua context (caught
/// by a script-level `pcall`).  `err:true:false` proves both: `true` = pcall
/// reported the error, `false` = the unprotected call's error propagated and the
/// Lua `pcall` returned not-ok.  Identical on all five legs.
#[test]
fn hostlua_hybrid_native_to_lua_callback_errors_parity() {
    require_sdk();
    require_lua_sdk();
    require_wasm();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("hostlua_hybrid_reverse_err");

    CartProject::new()
        .c(r#"#include "blyt.h"
/* pcall: catch the callee's error, report whether it errored. */
BLYT_LUA_MODULE_EXPORT_RAW(host, protected_call) {
    lua_getglobal(L, "boom");
    int rc = lua_pcall(L, 0, 0, 0);
    lua_pushboolean(L, rc != LUA_OK);
    return 1;
}
/* call: propagate the callee's error into the Lua caller (never returns here). */
BLYT_LUA_MODULE_EXPORT_RAW(host, unprotected_call) {
    lua_getglobal(L, "boom");
    lua_call(L, 0, 0);
    return 0;
}
"#)
        .lua(
            r#"
local host = require("host")
function boom()
    error("kaboom")
end
function init()
    local pc = host.protected_call()        -- true: pcall caught boom's error
    local ok = pcall(host.unprotected_call) -- false: lua_call re-raised, caught here
    blyt.debug.print("err:" .. tostring(pc) .. ":" .. tostring(ok))
end
function update() blyt.quit() end
function draw() end
"#,
        )
        .write(&project);

    let cart = build_lua_cart(&project);
    let expected = "err:true:false";

    run_cart_native(&cart, expected);
    run_cart_native_with_env(&cart, &[("BLYT_HOSTLUA", "1")], expected);
    run_cart_wasm(&cart, expected);
    run_cart_libretro(&cart, expected);
    run_cart_libretro_with_env(&cart, &[("BLYT_HOSTLUA", "1")], expected);
}

/// S6 — RE-ENTRANT reverse-trampoline (#262): native `host.outer()` calls Lua
/// `middle()`, which calls native `host.inner()` back — native→Lua→native.  On
/// the host-Lua legs the single rv32 CPU drives both native calls, so the nested
/// call must save/restore the outer call's CPU state or the outer's post-call
/// arithmetic reads garbage (anti-#98 teeth).  `re:211` = inner(5)=105, *2=210
/// in middle, +1 in outer after it resumes.  On bare-metal / emulated it is the
/// natural in-machine stack; the host-Lua legs must match it exactly.
#[test]
fn hostlua_hybrid_reentrant_native_to_lua_parity() {
    require_sdk();
    require_lua_sdk();
    require_wasm();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("hostlua_hybrid_reentrant");

    CartProject::new()
        .c(r#"#include "blyt.h"
BLYT_LUA_MODULE_EXPORT_RAW(host, inner) {
    lua_Integer x = lua_tointeger(L, 1);
    lua_pushinteger(L, x + 100);
    return 1;
}
/* Reverse-trampoline that RE-ENTERS native: outer -> middle (Lua) -> inner. */
BLYT_LUA_MODULE_EXPORT_RAW(host, outer) {
    lua_getglobal(L, "middle");
    lua_pushinteger(L, 5);
    lua_pcall(L, 1, 1, 0);            /* middle(5) calls host.inner internally */
    lua_Integer r = lua_tointeger(L, -1);
    lua_pushinteger(L, r + 1);        /* proves the outer call resumed cleanly */
    return 1;
}
"#)
        .lua(
            r#"
local host = require("host")
function middle(n)
    return host.inner(n) * 2
end
function init()
    blyt.debug.print("re:" .. host.outer())
end
function update() blyt.quit() end
function draw() end
"#,
        )
        .write(&project);

    let cart = build_lua_cart(&project);
    let expected = "re:211";

    run_cart_native(&cart, expected);
    run_cart_native_with_env(&cart, &[("BLYT_HOSTLUA", "1")], expected);
    run_cart_wasm(&cart, expected);
    run_cart_libretro(&cart, expected);
    run_cart_libretro_with_env(&cart, &[("BLYT_HOSTLUA", "1")], expected);
}

// ── #250: hybrid heap accounting + unified 16 MB budget across both halves ────
//
// A hybrid's Lua half runs on the host-Lua VM (its rv32-shadow arena) while the
// native C/Rust half stays EMULATED in an rv32 bridge session with its OWN
// libblytc arena (ADR-0130). #232 v1 left the two arenas independent, so:
//   * mem.stats().cart_allocations reported only the Lua-shadow figure — the
//     emulated native half's allocations were invisible; and
//   * the 16 MB unified budget (ADR-0008 / #158) was enforced per-arena, so a
//     hybrid's COMBINED footprint could exceed 16 MB undetected.
//
// #250 folds the native half's arena into the reported figure and makes the
// fail-point account for both halves together — modelling the SINGLE logical
// 16 MB pool bare-metal rv32 already shares natively. The emulated leg runs the
// whole cart in one arena (unified for free) and is being retired (ADR-0136), so
// it is NOT the byte-exact anchor here; the bar is the THREE host-Lua legs
// identical to each other (native host-Lua / WASM / libretro host-Lua), which
// all separate the two arenas identically and drive the same emulated native
// half through the same shared runner (#242). Anti-#98: assert the SAME observed
// value across all three, not per-leg smoke.

/// Parse the integer after `key=` on the line containing `key=` (e.g. the
/// `cart_allocations` figure a probe cart prints).
fn probe_u64(output: &str, key: &str) -> u64 {
    let needle = format!("{key}=");
    let line = output
        .lines()
        .find(|l| l.contains(&needle))
        .unwrap_or_else(|| panic!("no {needle:?} line in output:\n{output}"));
    let rest = &line[line.find(&needle).unwrap() + needle.len()..];
    let digits: String = rest.chars().take_while(|c| c.is_ascii_digit()).collect();
    digits
        .parse::<u64>()
        .unwrap_or_else(|_| panic!("bad {needle:?} value in line {line:?}"))
}

/// #250 (1) — the emulated native half's heap is FOLDED into
/// `mem.stats().cart_allocations`. The native half grabs a large, known block
/// (1 MiB) in its rv32 libblytc arena and holds it; the Lua half then reads
/// `cart_allocations`. Before the fold that figure reflected only the Lua-shadow
/// arena (a few KB), so it MUST now be >= the native block — and byte-identical
/// across the three host-Lua legs, which run the identical emulated native half
/// through the identical shared runner (#242/#267: the first-fit count is in the
/// ADR-0029 contract, so the fold's ORDER — not just its total — must match).
#[test]
fn hostlua_hybrid_native_heap_folded_into_mem_stats() {
    require_sdk();
    require_lua_sdk();
    require_wasm();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("hostlua_hybrid_fold");

    CartProject::new()
        .c(r#"#include "blyt.h"
#include <stdlib.h>
/* The emulated native half allocates a known block in its rv32 libblytc arena
 * and holds it live for the cart's lifetime, so it stays counted in the
 * session's guest_heap_used. */
static void *g_block;
BLYT_LUA_MODULE_EXPORT_RAW(host, grab) {
    lua_Integer n = lua_tointeger(L, 1);
    g_block = malloc((size_t)n);
    if (g_block)
        ((volatile char *)g_block)[0] = 1; /* touch: not dead-stripped */
    lua_pushboolean(L, g_block != NULL);
    return 1;
}
"#)
        .lua(
            r#"
local host = require("host")
local MIB = 1024 * 1024
function init()
    local ok = host.grab(MIB) -- 1 MiB in the emulated native half's arena
    collectgarbage("collect")
    local m = blyt32.mem.stats()
    blyt.debug.print(string.format("FOLD cart_allocations=%d grab=%s", m.cart_allocations,
        tostring(ok)))
end
function update() blyt.quit() end
function draw() end
"#,
        )
        .write(&project);

    let cart = build_lua_cart(&project);

    let native = probe_u64(
        &capture_cart_native(&cart, &[("BLYT_HOSTLUA", "1")]),
        "cart_allocations",
    );
    let wasm = probe_u64(&capture_cart_wasm(&cart, &[]), "cart_allocations");
    let libretro = probe_u64(
        &capture_cart_libretro(&cart, &[("BLYT_HOSTLUA", "1")]),
        "cart_allocations",
    );

    // The native half's 1 MiB block must now be visible in cart_allocations
    // (before the fold it reflected only the Lua-shadow arena, << 1 MiB).
    assert!(
        native >= 1024 * 1024,
        "native host-Lua cart_allocations ({native}) must include the emulated \
         native half's 1 MiB block (#250 fold); a value < 1 MiB means the native \
         arena is not folded into mem.stats()"
    );
    // Byte-identical across the three host-Lua legs (anti-#98).
    assert_eq!(
        native, wasm,
        "hybrid cart_allocations must be byte-identical native-host-Lua vs wasm32 \
         (#250/#267): native={native}, wasm32={wasm}"
    );
    assert_eq!(
        native, libretro,
        "hybrid cart_allocations must be byte-identical native-host-Lua vs \
         libretro-host-Lua (#250): native={native}, libretro={libretro}"
    );
}

/// #250 (2) — the unified 16 MB budget (ADR-0008 / #158) fail-point accounts for
/// BOTH halves together. The Lua half holds ~8 MiB live; the native half then
/// tries to grab 9 MiB. Each fits its OWN arena (< 16 MiB), but the COMBINED
/// 17 MiB exceeds the single logical 16 MiB pool, so the native grab MUST fail —
/// deterministically and identically on every host-Lua leg. Before the shared
/// fail-point the native arena would allow it (9 < 16) and the cart would report
/// success, silently over-budget.
#[test]
fn hostlua_hybrid_unified_budget_fail_point_parity() {
    require_sdk();
    require_lua_sdk();
    require_wasm();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("hostlua_hybrid_budget");

    CartProject::new()
        .c(r#"#include "blyt.h"
#include <stdlib.h>
static void *g_block;
BLYT_LUA_MODULE_EXPORT_RAW(host, grab) {
    lua_Integer n = lua_tointeger(L, 1);
    g_block = malloc((size_t)n);
    if (g_block)
        ((volatile char *)g_block)[0] = 1;
    lua_pushboolean(L, g_block != NULL);
    return 1;
}
"#)
        .lua(
            r#"
local host = require("host")
local MIB = 1024 * 1024
local KEEP = {}
function init()
    -- Lua half: ~8 MiB of live, non-interned string bodies held in KEEP.
    for i = 1, 8 do
        KEEP[i] = string.rep("x", MIB)
    end
    collectgarbage("collect")
    -- Native half: 9 MiB. Alone it fits the 16 MiB arena; combined with the
    -- Lua half's ~8 MiB it exceeds the unified 16 MiB pool and MUST fail.
    local ok = host.grab(9 * MIB)
    blyt.debug.print("BUDGET grab_ok=" .. tostring(ok))
end
function update() blyt.quit() end
function draw() end
"#,
        )
        .write(&project);

    let cart = build_lua_cart(&project);
    let expected = "BUDGET grab_ok=false";

    // Deterministic, leg-identical over-budget rejection across the three
    // host-Lua legs (anti-#98). A green here without the shared fail-point would
    // print grab_ok=true (native arena allows 9 MiB on its own).
    run_cart_native_with_env(&cart, &[("BLYT_HOSTLUA", "1")], expected);
    run_cart_wasm(&cart, expected);
    run_cart_libretro_with_env(&cart, &[("BLYT_HOSTLUA", "1")], expected);
}

// ── #278: hybrid must reserve its persistent footprint during init() ─────────
//
// #250 coupled the two hybrid arenas (host-VM Lua half + emulated rv32 native
// half) ONLY at trampoline points: hl_budget_enter/leave_native + pin/unpin
// publish the peer heap and fold the session's cart heap + persistent-resource
// footprint into the host-VM arena's non_evictable_footprint. Nothing published
// at VM-build time, so a hybrid's host-VM arena started with footprint = 0 and
// ignored the session's persistent reservation until the FIRST native call.
//
// A Lua init() that fills the heap BEFORE touching native therefore ran against
// the full 16 MiB on the host-Lua legs, but against only (16 - persistent) MiB on
// bare metal / the emulated oracle, which reserves persistent from frame 0
// (ADR-0028) — the single-arena model bare metal runs natively. That is a
// bare-metal determinism divergence (ADR-0136 anchor), not merely an
// emulated-vs-host-Lua one, so the teeth pin the host-Lua legs to the emulated
// rv32 oracle here and to real RISC-V hardware in the QEMU gate (native_qemu.rs).

#[test]
fn hostlua_hybrid_init_budget_reserves_persistent_parity() {
    require_sdk();
    require_lua_sdk();
    require_wasm();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("hostlua_hybrid_init_budget");
    CartProject::new()
        .c(common::INIT_BUDGET_HYBRID_C)
        .lua(common::INIT_BUDGET_HYBRID_LUA)
        .asset_bytes("pers8.bin", &vec![0u8; common::INIT_BUDGET_PERSIST_BYTES])
        .persistent(&["pers8"])
        .write(&project);

    let cart = build_lua_cart(&project);
    let golden = common::INIT_BUDGET_FILLED;

    // The emulated rv32 leg (no BLYT_HOSTLUA): a hybrid defaults to emulated
    // (ADR-0136), which reserves the 8 MiB persistent set from frame 0 (ADR-0028)
    // — the same one-arena model bare metal runs. This cross-checks the pinned
    // golden against the oracle, so a scaffolding shift is caught here (not only
    // in the QEMU gate, native_qemu.rs, which pins the same figure on hardware).
    let oracle = probe_u64(&capture_cart_native(&cart, &[]), "filled");
    assert_eq!(
        oracle, golden,
        "emulated rv32 oracle filled={oracle} != pinned INIT_BUDGET_FILLED={golden}: \
         the 8 MiB persistent reservation moved — update the shared const and the \
         QEMU gate together (#278)"
    );

    // Every host-Lua leg must reserve the SAME persistent footprint from frame 0.
    // RED before the #278 fix: footprint = 0 until the first trampoline, so the
    // fill runs against the full 16 MiB and `filled` is ~255 (2x the golden).
    let native = probe_u64(
        &capture_cart_native(&cart, &[("BLYT_HOSTLUA", "1")]),
        "filled",
    );
    let wasm = probe_u64(&capture_cart_wasm(&cart, &[]), "filled");
    let libretro = probe_u64(
        &capture_cart_libretro(&cart, &[("BLYT_HOSTLUA", "1")]),
        "filled",
    );

    assert_eq!(
        native, golden,
        "native host-Lua hybrid must reserve its persistent footprint during init() \
         from frame 0 like bare metal (#278): native filled={native}, expected={golden}"
    );
    assert_eq!(
        wasm, golden,
        "WASM host-Lua hybrid must reserve its persistent footprint during init() \
         from frame 0 like bare metal (#278): wasm filled={wasm}, expected={golden}"
    );
    assert_eq!(
        libretro, golden,
        "libretro host-Lua hybrid must reserve its persistent footprint during init() \
         from frame 0 like bare metal (#278): libretro filled={libretro}, expected={golden}"
    );
}

// ── #280: the reservation must survive the --reset-every-frame rebuild ───────
//
// #278 fixed the INITIAL-LOAD path only (hl_new / run_lua_cart). The shared
// VM-rebuild path was left unseeded on purpose, so on a reset cycle the rebuilt
// VM re-runs init() against a zeroed non_evictable_footprint until its first
// native trampoline — a hybrid that reallocates in the re-run init() before
// touching native ignores its own persistent reservation all over again.
//
// Bare metal keeps persistent reserved across the reset (ADR-0028); the emulated
// rv32 legs model that one-arena behaviour and are the oracle here. Bare metal
// itself has no --reset-every-frame to gate on directly, so the hardware anchor
// is transitive: every cycle must reproduce the frame-0 figure, and frame 0 is
// pinned to real RISC-V hardware by Gate 8c (native_qemu.rs) on the sibling #278
// cart. That is why this asserts against INIT_BUDGET_FILLED rather than a fresh
// golden — the two probes must agree, because "the reset cycle sees the same
// headroom as frame 0" IS the invariant under test.
//
// RED before the fix: the first line is the (already-fixed) frame-0 figure and
// every later line is ~2x it, the full 16 MiB the unseeded rebuild hands back.

/// Every `filled=` value the probe printed — one per init(), i.e. frame 0 plus
/// one per reset cycle.
fn probe_all_u64(output: &str, key: &str) -> Vec<u64> {
    let needle = format!("{key}=");
    output
        .lines()
        .filter_map(|l| l.find(&needle).map(|i| &l[i + needle.len()..]))
        .map(|rest| {
            rest.chars()
                .take_while(|c| c.is_ascii_digit())
                .collect::<String>()
                .parse::<u64>()
                .unwrap_or_else(|_| panic!("bad {needle:?} value in {rest:?}"))
        })
        .collect()
}

/// Assert the probe was actually reset (more than one init()) and that every
/// cycle saw the same budget headroom as frame 0.
fn assert_reset_budget(leg: &str, output: &str, golden: u64) {
    let filled = probe_all_u64(output, "filled");
    assert!(
        filled.len() >= 2,
        "{leg}: expected >=2 RESETBUDGET lines (frame 0 + at least one reset cycle), \
         got {filled:?}\noutput:\n{output}"
    );
    for (cycle, n) in filled.iter().enumerate() {
        assert_eq!(
            *n, golden,
            "{leg}: init() on cycle {cycle} saw filled={n}, expected {golden} — the \
             rebuilt VM must re-reserve the hybrid's persistent footprint before \
             init() runs, like frame 0 and bare metal (#280/#278). All cycles: {filled:?}"
        );
    }
}

#[test]
fn hostlua_hybrid_reset_cycle_budget_reserves_persistent_parity() {
    require_sdk();
    require_lua_sdk();
    require_wasm();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("hostlua_hybrid_reset_budget");
    CartProject::new()
        .c(common::RESET_BUDGET_HYBRID_C)
        .config(common::RESET_BUDGET_HYBRID_CONFIG)
        .lua(common::RESET_BUDGET_HYBRID_LUA)
        .asset_bytes("pers8.bin", &vec![0u8; common::INIT_BUDGET_PERSIST_BYTES])
        .persistent(&["pers8"])
        .write(&project);

    let cart = build_lua_cart(&project);
    let golden = common::INIT_BUDGET_FILLED;

    // The emulated rv32 oracle (no BLYT_HOSTLUA): one arena, persistent reserved
    // from frame 0 and kept across the reset (ADR-0028) — the model bare metal
    // runs. It must hold that reservation on EVERY cycle, which before #280 it
    // did not: re-running init() built a fresh guest Lua state without reclaiming
    // the abandoned one, so the leak ate ~34 KB of headroom per cycle and the
    // count decayed 127 → 126 → 125.
    assert_reset_budget(
        "emulated rv32 oracle",
        &common::capture_cart_native_with_flags(&cart, &["--reset-every-frame"], &[]),
        golden,
    );

    // The three host-Lua legs must hold the same reservation across the rebuild.
    assert_reset_budget(
        "native host-Lua",
        &common::capture_cart_native_with_flags(
            &cart,
            &["--reset-every-frame"],
            &[("BLYT_HOSTLUA", "1")],
        ),
        golden,
    );
    assert_reset_budget(
        "wasm host-Lua",
        &common::capture_cart_wasm(&cart, &[("BLYT_RESET_EVERY_FRAME", "1")]),
        golden,
    );
    assert_reset_budget(
        "libretro host-Lua",
        &common::capture_cart_libretro_with_flags(
            &cart,
            &["--reset-every-frame"],
            &[("BLYT_HOSTLUA", "1")],
        ),
        golden,
    );
}

/// #280 (2) — the WASM leg's rebuild never recaptured its scaffolding baseline,
/// so after ANY reset/reload `cart_allocations` still counted the whole rebuilt
/// VM + stdlibs + API + bytecode as cart heap while the native leg (whose
/// `build_vm` recaptures) counted 0. A #267-class cross-leg divergence in a
/// cart-visible figure (ADR-0029), independent of the budget reservation above:
/// it is wrong even for a cart that never gets near the 16 MB cap.
///
/// The cart holds a known 256 KiB live across the reset cycle and reports
/// `cart_allocations` once the cycle has run, so the figure covers a rebuilt VM
/// rather than the initial load. RED on WASM before the fix (scaffolding-inflated
/// by tens of KB); the three host-Lua legs must agree byte-for-byte.
#[test]
fn hostlua_hybrid_reset_cycle_cart_allocations_parity() {
    require_sdk();
    require_lua_sdk();
    require_wasm();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("hostlua_hybrid_reset_alloc");
    CartProject::new()
        .c(common::RESET_BUDGET_HYBRID_C)
        .config(common::RESET_BUDGET_HYBRID_CONFIG)
        .lua(
            r#"
require("host") -- hybrid marker
local KEEP = {}
local frame = 0

function init()
    -- A known, deterministic live set: 4 x 64 KiB distinct long strings.
    KEEP = {}
    for i = 1, 4 do
        KEEP[i] = string.rep(tostring(i), 64 * 1024)
    end
    collectgarbage("collect")
end

function on_new_state()
    blyt.buf.alloc_slot(S.GLOBALS)
end

function update()
    frame = frame + 1
    -- Report from frame 2, i.e. after at least one reset cycle has rebuilt the
    -- VM — the initial load already agreed across legs before #280.
    if frame >= 2 then
        blyt.debug.print("RESETALLOC alloc=" .. blyt32.mem.stats().cart_allocations)
        blyt.quit()
    end
end

function draw() end
function on_save_state() S.globals[0].frame = frame end
function on_load_state(_info) frame = S.globals[0].frame end
"#,
        )
        .write(&project);

    let cart = build_lua_cart(&project);

    let native = probe_u64(
        &common::capture_cart_native_with_flags(
            &cart,
            &["--reset-every-frame"],
            &[("BLYT_HOSTLUA", "1")],
        ),
        "alloc",
    );
    let wasm = probe_u64(
        &common::capture_cart_wasm(&cart, &[("BLYT_RESET_EVERY_FRAME", "1")]),
        "alloc",
    );
    let libretro = probe_u64(
        &common::capture_cart_libretro_with_flags(
            &cart,
            &["--reset-every-frame"],
            &[("BLYT_HOSTLUA", "1")],
        ),
        "alloc",
    );

    assert!(
        native >= 256 * 1024,
        "native host-Lua cart_allocations ({native}) must include the cart's own \
         256 KiB live set after the reset cycle"
    );
    assert_eq!(
        native, wasm,
        "cart_allocations after a reset cycle must be byte-identical native-host-Lua \
         vs wasm32 (#280/#267): native={native}, wasm32={wasm} — a wasm value inflated \
         by tens of KB means wasm_lua_rebuild did not recapture its scaffolding baseline"
    );
    assert_eq!(
        native, libretro,
        "cart_allocations after a reset cycle must be byte-identical native-host-Lua \
         vs libretro-host-Lua (#280): native={native}, libretro={libretro}"
    );
}

// ── #276: hybrid legs must resolve ONE cart-facing resource table ─────────────
//
// A hybrid's Lua half runs on the host-Lua VM; its native C/Rust half is emulated
// in the rv32 bridge session (ADR-0130). Resource ops from the native half go
// through the resource ECALL handlers into the SESSION's ctx.resources. The
// cart-facing `blyt.resource.*` + `blyt32.mem.stats()` bindings the Lua half reads
// must therefore resolve that SAME session table — otherwise the native half's
// residency/pin work is invisible to the Lua half on one leg but not another.
//
// The bug (#276): the native host-Lua leg (cart_run_hostlua.c) read the RUNNER's
// own `hl->resources` for a hybrid, while WASM (wasm_main.c) read the session's
// `ctx.resources`. So a hybrid whose native half pins a resource reported divergent
// `mem.stats().resource_cache_used` / `resources_loaded` and diverging residency
// across the native vs WASM (and libretro) host-Lua legs — a latent anti-#98
// cross-leg divergence (determinism contract, ADR-0007/0029). The fix unifies the
// native leg onto the session's table (matching WASM), single-sourcing the hybrid's
// one cart-facing resource table.

/// #276 teeth — a hybrid whose EMULATED NATIVE half pins a cart resource (into the
/// session's resource table). The Lua half then reads `blyt32.mem.stats()`, which
/// must resolve that SAME session table on every host-Lua leg, so the native-half
/// pin is visible as `resource_cache_used` / a `resources_loaded` entry identically
/// across native / WASM / libretro host-Lua (anti-#98).
///
/// Before the fix the native (and libretro) host-Lua leg read the runner-owned
/// `hl->resources` — untouched by the native-half pin — so it reported the resource
/// as NOT resident (cache_used=0, listed=0), diverging from WASM which read the
/// session table (resident). The cross-leg equality asserts catch that divergence;
/// the positive `listed==1` assert prevents a trivially-equal all-zero green.
#[test]
fn hostlua_hybrid_native_half_pins_resource_table_parity() {
    require_sdk();
    require_lua_sdk();
    require_wasm();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("hostlua_hybrid_restable");

    CartProject::new()
        .c(r#"#include "blyt.h"
#include "cart_resources.h" /* generated R_<NAME> constants */
/* The emulated native half pins the cart resource and HOLDS it for the cart's
 * lifetime, so it stays resident in the rv32 session's resource table — the ONE
 * table the Lua half's mem.stats() must read on every host-Lua leg (#276). */
static const void *g_p;
static size_t g_sz;
BLYT_LUA_MODULE_EXPORT_RAW(host, pin_big) {
    int ok = (blyt_resource_pin((blyt_resource_id_t)R_BIG, &g_p, &g_sz) == BLYT_OK);
    lua_pushboolean(L, ok);
    return 1;
}
"#)
        .lua(
            r#"
local host = require("host")
local R = require("cart_resources")
function init()
    local ok = host.pin_big() -- native half pins R.BIG into the session table
    local m = blyt32.mem.stats()
    local listed = 0
    for _, r in ipairs(m.resources_loaded) do
        if r.id == R.BIG:id() then listed = 1 end
    end
    blyt.debug.print(string.format("RESTABLE pin=%s cache_used=%d listed=%d loaded=%d",
        tostring(ok), m.resource_cache_used, listed, #m.resources_loaded))
end
function update() blyt.quit() end
function draw() end
"#,
        )
        // 64 KiB of zeros: the packer compresses it (#157), so the native-half pin
        // decompresses it into an owned buffer -> it counts in resident_decompressed
        // (resource_cache_used) and appears in resources_loaded.
        .asset_bytes("big.bin", &[0u8; 64 * 1024])
        .write(&project);

    let cart = build_lua_cart(&project);

    let native = capture_cart_native(&cart, &[("BLYT_HOSTLUA", "1")]);
    let wasm = capture_cart_wasm(&cart, &[]);
    let libretro = capture_cart_libretro(&cart, &[("BLYT_HOSTLUA", "1")]);

    let (n_cache, n_listed, n_loaded) = (
        probe_u64(&native, "cache_used"),
        probe_u64(&native, "listed"),
        probe_u64(&native, "loaded"),
    );
    let (w_cache, w_listed, w_loaded) = (
        probe_u64(&wasm, "cache_used"),
        probe_u64(&wasm, "listed"),
        probe_u64(&wasm, "loaded"),
    );
    let (l_cache, l_listed, l_loaded) = (
        probe_u64(&libretro, "cache_used"),
        probe_u64(&libretro, "listed"),
        probe_u64(&libretro, "loaded"),
    );

    // The native-half pin must be visible to the Lua half's mem.stats() — a
    // meaningful (non-trivial) green: the resource is resident and listed, not the
    // all-zero case that would satisfy equality without proving anything (anti-#98).
    assert_eq!(
        w_listed, 1,
        "the native half pinned R.BIG into the session table; the Lua half's \
         mem.stats().resources_loaded must list it on WASM (the reference leg): {wasm}"
    );
    assert!(
        w_cache >= 64 * 1024,
        "WASM resource_cache_used ({w_cache}) must include the pinned 64 KiB resource: {wasm}"
    );

    // Byte-identical resource residency across all three host-Lua legs (anti-#98):
    // native and libretro must read the SAME session table WASM does, so the
    // native-half pin is equally visible. Before the #276 fix the native/libretro
    // legs read the runner-owned table (pin invisible) and diverge here.
    assert_eq!(
        (n_cache, n_listed, n_loaded),
        (w_cache, w_listed, w_loaded),
        "hybrid mem.stats resource figures must be identical native-host-Lua vs \
         wasm32 (#276): native=(cache={n_cache},listed={n_listed},loaded={n_loaded}) \
         wasm=(cache={w_cache},listed={w_listed},loaded={w_loaded})\nnative:\n{native}\nwasm:\n{wasm}"
    );
    assert_eq!(
        (n_cache, n_listed, n_loaded),
        (l_cache, l_listed, l_loaded),
        "hybrid mem.stats resource figures must be identical native-host-Lua vs \
         libretro-host-Lua (#276): native=(cache={n_cache},listed={n_listed},loaded={n_loaded}) \
         libretro=(cache={l_cache},listed={l_listed},loaded={l_loaded})\nnative:\n{native}\nlibretro:\n{libretro}"
    );
}
