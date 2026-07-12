mod common;

use assert_cmd::Command;
use common::{
    CartProject, blytdebug, build_debug_lua_cart, build_lua_cart, find_wasm_debug_dir, repo_root,
    require_gdb, require_lua_sdk, require_sdk, require_wasm_debug,
};
use tempfile::TempDir;

/// WASM DAP gate test: set a breakpoint in a Lua cart running inside blytplay.js,
/// verify the stopped event, inspect locals, step over, and continue to completion.
///
/// Requires the debug WASM runtime (BLYT_DAP=ON) to have been built:
///   cmake --build build --target sdk   (requires emcc)
/// (incremental rebuild: cmake --build build/build-wasm-debug)
///
/// Silently asserts on missing WASM runtime or Lua SDK.
#[test]
fn wasm_dap_breakpoint_step_inspect() {
    require_wasm_debug();
    require_lua_sdk();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("dap_lua_cart");

    // Breakpoint at line 4 (default BLYT_DAP_BP_LINE=4).
    // The Lua source is compiled as main.lua; run_dap_test.mjs passes that
    // basename to dap_test.mjs as the breakpoint source path.
    // Uses blyt_quit() (the WASM Lua API) not blyt.quit() (emulated path).
    CartProject::new()
        .lua(
            "function init()\n\
             \x20   local x = 42\n\
             \x20   local y = x + 1\n\
             \x20   local z = y + 1\n\
             end\n\
             function update() blyt_quit() end\n\
             function draw() end\n",
        )
        .write(&project);

    let cart = build_lua_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let wasm_dir = find_wasm_debug_dir();
    let orchestrator = repo_root().join("tests/dap/run_dap_test.mjs");

    Command::new("node")
        .args([
            orchestrator.to_str().unwrap(),
            wasm_dir.to_str().unwrap(),
            cart.to_str().unwrap(),
        ])
        .assert()
        .success();
}

/// WASM DAP: evaluate — assert that arbitrary Lua expressions are evaluated in
/// the frame context.  Stops at line 3 (x = 42 in scope); evaluates "x + 1"
/// and expects "43".  Covers the handle_evaluate / dap_evaluating fix.
#[test]
fn wasm_dap_evaluate_expr() {
    require_wasm_debug();
    require_lua_sdk();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("wasm_dap_evaluate");
    CartProject::new()
        .lua(
            "function init()\n\
             \x20   local x = 42\n\
             \x20   local y = x + 1\n\
             \x20   local z = y + 1\n\
             end\n\
             function update() blyt_quit() end\n\
             function draw() end\n",
        )
        .write(&project);

    let cart = build_lua_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let wasm_dir = find_wasm_debug_dir();
    let orchestrator = repo_root().join("tests/dap/run_dap_test.mjs");

    Command::new("node")
        .args([
            orchestrator.to_str().unwrap(),
            wasm_dir.to_str().unwrap(),
            cart.to_str().unwrap(),
        ])
        .env("BLYT_DAP_BP_LINE", "3")
        .env("BLYT_DAP_EVALUATE_EXPR", "x + 1")
        .env("BLYT_DAP_EVALUATE_EXPECT", "43")
        .assert()
        .success();
}

/// WASM DAP: conditional breakpoint — stop only when the Lua condition is true.
/// Stops at line 3 only when x > 10 (x = 42, so it fires on the first pass).
#[test]
fn wasm_dap_conditional_breakpoint() {
    require_wasm_debug();
    require_lua_sdk();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("wasm_dap_conditional_bp");
    CartProject::new()
        .lua(
            "function init()\n\
             \x20   local x = 42\n\
             \x20   local y = x + 1\n\
             \x20   local z = y + 1\n\
             end\n\
             function update() blyt_quit() end\n\
             function draw() end\n",
        )
        .write(&project);

    let cart = build_lua_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let wasm_dir = find_wasm_debug_dir();
    let orchestrator = repo_root().join("tests/dap/run_dap_test.mjs");

    Command::new("node")
        .args([
            orchestrator.to_str().unwrap(),
            wasm_dir.to_str().unwrap(),
            cart.to_str().unwrap(),
        ])
        .env("BLYT_DAP_BP_LINE", "3")
        .env("BLYT_DAP_CONDITIONAL_COND", "x > 10")
        .assert()
        .success();
}

/// WASM DAP: condition editing — set a conditional BP, stop once, then update the
/// condition while paused and verify the new condition controls the next stop.
///
/// Cart loops `for i = 1, 10` with `local x = i` on each iteration (line 3).
/// Initial condition: `x == 3` → first stop at i=3.
/// Edited condition: `x == 7` → second stop at i=7; verifies the edit took effect.
#[test]
fn wasm_dap_conditional_breakpoint_edit() {
    require_wasm_debug();
    require_lua_sdk();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("wasm_dap_cond_edit");
    // BP is at line 4 (local dummy = x), where x was assigned at line 3 and is
    // already in scope.  Condition "x == 3" fires on the 3rd iteration; after
    // the edit "x == 7" fires on the 7th.
    CartProject::new()
        .lua(
            "function init()\n\
             \x20   for i = 1, 10 do\n\
             \x20       local x = i\n\
             \x20       local dummy = x\n\
             \x20   end\n\
             end\n\
             function update() blyt_quit() end\n\
             function draw() end\n",
        )
        .write(&project);

    let cart = build_lua_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let wasm_dir = find_wasm_debug_dir();
    let orchestrator = repo_root().join("tests/dap/run_dap_test.mjs");

    Command::new("node")
        .args([
            orchestrator.to_str().unwrap(),
            wasm_dir.to_str().unwrap(),
            cart.to_str().unwrap(),
        ])
        .env("BLYT_DAP_BP_LINE", "4")
        .env("BLYT_DAP_CONDITIONAL_COND", "x == 3")
        .env("BLYT_DAP_CONDITIONAL_COND_EDIT", "x == 7")
        .env("BLYT_DAP_COND_EDIT_STOP_VAR", "x")
        .env("BLYT_DAP_COND_EDIT_STOP_VAL", "7")
        .assert()
        .success();
}

/// SDL2 DAP gate: set a breakpoint in a Lua cart running under blytplay --debug
/// --headless, connect a TCP DAP client, verify stopped / step / continue.
///
/// Requires the native build (blytplay + libblyt32lua.so) and Lua SDK.
/// Uses BLYT_DAP_BP_LINE=3 (third line of init(), "local y = x + 1").
#[test]
fn sdl_dap_breakpoint_step_inspect() {
    require_sdk();
    require_lua_sdk();
    assert!(
        blytdebug().exists(),
        "blytplay not built — run `cmake --build build` first"
    );

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("sdl_dap_lua");
    CartProject::new()
        .lua(
            "function init()\n\
             \x20   local x = 42\n\
             \x20   local y = x + 1\n\
             \x20   local z = y + 1\n\
             end\n\
             function update() blyt.quit() end\n\
             function draw() end\n",
        )
        .write(&project);

    let cart = build_lua_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let orchestrator = repo_root().join("tests/dap/run_sdl_dap_test.mjs");
    Command::new("node")
        .args([
            orchestrator.to_str().unwrap(),
            blytdebug().to_str().unwrap(),
            cart.to_str().unwrap(),
        ])
        .env("BLYT_DAP_BP_LINE", "3")
        .assert()
        .success();
}

/// Native host-Lua DAP gate (#234): the SAME breakpoint/step/inspect flow as
/// `sdl_dap_breakpoint_step_inspect`, but with BLYT_HOSTLUA=1 so blytdebug runs
/// the cart in the native host-Lua VM (cart_run_hostlua.c) instead of the rv32
/// emulated Lua VM.  Exercises the native TCP DAP transport + shared inspection
/// core against a real host lua_State — parity with the emulated + WASM legs.
///
/// (On x86-64/arm64 the deterministic seam VM is always compiled into
/// libblyt_debug, so BLYT_HOSTLUA selects the host-Lua path here; a build
/// without it would transparently fall back to the emulated path.)
#[test]
fn sdl_dap_hostlua_breakpoint_step_inspect() {
    require_sdk();
    require_lua_sdk();
    assert!(
        blytdebug().exists(),
        "blytdebug not built — run `cmake --build build` first"
    );

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("sdl_dap_hostlua");
    CartProject::new()
        .lua(
            "function init()\n\
             \x20   local x = 42\n\
             \x20   local y = x + 1\n\
             \x20   local z = y + 1\n\
             end\n\
             function update() blyt.quit() end\n\
             function draw() end\n",
        )
        .write(&project);

    let cart = build_lua_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let orchestrator = repo_root().join("tests/dap/run_sdl_dap_test.mjs");
    Command::new("node")
        .args([
            orchestrator.to_str().unwrap(),
            blytdebug().to_str().unwrap(),
            cart.to_str().unwrap(),
        ])
        .env("BLYT_DAP_BP_LINE", "3")
        .env("BLYT_HOSTLUA", "1")
        .assert()
        .success();
}

/// Native host-Lua DAP (#234): conditional breakpoint parity — the local `x` is
/// visible to the condition, evaluated in-frame by the shared inspection core
/// against the host lua_State (mirrors sdl_dap_conditional_breakpoint / the WASM
/// leg).
#[test]
fn sdl_dap_hostlua_conditional_breakpoint() {
    require_sdk();
    require_lua_sdk();
    assert!(blytdebug().exists(), "blytdebug not built");

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("sdl_dap_hostlua_cond");
    CartProject::new()
        .lua(
            "function init()\n\
             \x20   local x = 42\n\
             \x20   local y = x + 1\n\
             \x20   local z = y + 1\n\
             end\n\
             function update() blyt.quit() end\n\
             function draw() end\n",
        )
        .write(&project);

    let cart = build_lua_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let orchestrator = repo_root().join("tests/dap/run_sdl_dap_test.mjs");
    Command::new("node")
        .args([
            orchestrator.to_str().unwrap(),
            blytdebug().to_str().unwrap(),
            cart.to_str().unwrap(),
        ])
        .env("BLYT_DAP_BP_LINE", "3")
        .env("BLYT_DAP_CONDITIONAL_COND", "x > 10")
        .env("BLYT_HOSTLUA", "1")
        .assert()
        .success();
}

/// Native host-Lua DAP (#234): evaluate parity — an arbitrary expression is
/// evaluated in the paused frame context (not just a name lookup), running a
/// nested lua_pcall on the parked host lua_State (mirrors sdl_dap_evaluate_expr).
#[test]
fn sdl_dap_hostlua_evaluate_expr() {
    require_sdk();
    require_lua_sdk();
    assert!(blytdebug().exists(), "blytdebug not built");

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("sdl_dap_hostlua_eval");
    CartProject::new()
        .lua(
            "function init()\n\
             \x20   local x = 42\n\
             \x20   local y = x + 1\n\
             \x20   local z = y + 1\n\
             end\n\
             function update() blyt.quit() end\n\
             function draw() end\n",
        )
        .write(&project);

    let cart = build_lua_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let orchestrator = repo_root().join("tests/dap/run_sdl_dap_test.mjs");
    Command::new("node")
        .args([
            orchestrator.to_str().unwrap(),
            blytdebug().to_str().unwrap(),
            cart.to_str().unwrap(),
        ])
        .env("BLYT_DAP_BP_LINE", "3")
        .env("BLYT_DAP_EVALUATE_EXPR", "x + 1")
        .env("BLYT_DAP_EVALUATE_EXPECT", "43")
        .env("BLYT_HOSTLUA", "1")
        .assert()
        .success();
}

/// Native host-Lua DAP (#253): inspection of an **f64 state-buffer field**. The
/// shared inspection core reads the field through the `S` proxy on the parked
/// host `lua_State`, so it exercises the same `get_f64` accessor the #235 bug
/// misrouted to `get_i32`. The cart stores a fractional f64 (`42.5`) into a
/// state-buffer field, breaks on the following line, and evaluates
/// `S.game[slot].health` — which must render as the exact `42.5`, not an
/// i32-misdecoded value. This closes the DAP-inspect leg of the never-exercised-
/// f64 audit (the host-Lua state-buffer DAP tests otherwise only touch i32/plain
/// locals). The inspection core is shared and value-generic, so its correctness
/// here rides on the f64 accessor being correct on the host-Lua path.
#[test]
fn sdl_dap_hostlua_f64_state_buffer_inspect() {
    require_sdk();
    require_lua_sdk();
    assert!(blytdebug().exists(), "blytdebug not built");

    const SB_F64_CONFIG: &str = "\
records:
  Game:
    fields:
      - { name: health, type: f64 }
state_buffers:
  game:
    record: Game
    count: 1
";

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("sdl_dap_hostlua_f64");
    CartProject::new()
        .config(SB_F64_CONFIG)
        .lua(
            "function init()\n\
             \x20   local slot = blyt.buf.alloc_slot(S.GAME)\n\
             \x20   S.game[slot].health = 42.5\n\
             \x20   local marker = slot\n\
             end\n\
             function update() blyt.quit() end\n\
             function draw() end\n",
        )
        .write(&project);

    let cart = build_lua_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    // Break on line 4 (`local marker = slot`) — line 3 has already stored 42.5
    // and `slot` is a live local in the paused frame, so the evaluate can index
    // the state buffer through the `S` proxy's f64 accessor.
    let orchestrator = repo_root().join("tests/dap/run_sdl_dap_test.mjs");
    Command::new("node")
        .args([
            orchestrator.to_str().unwrap(),
            blytdebug().to_str().unwrap(),
            cart.to_str().unwrap(),
        ])
        .env("BLYT_DAP_BP_LINE", "4")
        .env("BLYT_DAP_EVALUATE_EXPR", "S.game[slot].health")
        .env("BLYT_DAP_EVALUATE_EXPECT", "42.5")
        .env("BLYT_HOSTLUA", "1")
        .assert()
        .success();
}

/// Native host-Lua DAP for a HYBRID cart (#232 S6): a breakpoint in the Lua half,
/// set on the line AFTER it calls a native export (`add_one`, driven through the
/// ADR-0130 ECALL bridge), hits under blytdebug's host-Lua path, and the
/// native-derived local is evaluable in the paused frame (`x + 1 == 43`, with
/// `x == add_one(41) == 42`). Proves `blyt_hostlua_create_debug` builds the
/// bridge session AND arms the Lua master hook together — the Lua-half
/// source-debug path of a hybrid, parity with the emulated + WASM legs. Native-
/// half GDB is deferred to a follow-up (#232 close-out).
#[test]
fn sdl_dap_hostlua_hybrid_breakpoint_evaluate() {
    require_sdk();
    require_lua_sdk();
    assert!(blytdebug().exists(), "blytdebug not built");

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("sdl_dap_hostlua_hybrid");
    CartProject::new()
        .c("#include \"blyt.h\"\n\
             BLYT_LUA_EXPORT_I32(add_one, int32_t x) { return x + 1; }\n")
        .lua(
            "function init()\n\
             \x20   local x = add_one(41)\n\
             \x20   local y = x + 1\n\
             \x20   local z = y + 1\n\
             end\n\
             function update() blyt.quit() end\n\
             function draw() end\n",
        )
        .write(&project);

    let cart = build_lua_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let orchestrator = repo_root().join("tests/dap/run_sdl_dap_test.mjs");
    Command::new("node")
        .args([
            orchestrator.to_str().unwrap(),
            blytdebug().to_str().unwrap(),
            cart.to_str().unwrap(),
        ])
        .env("BLYT_DAP_BP_LINE", "3")
        .env("BLYT_DAP_EVALUATE_EXPR", "x + 1")
        .env("BLYT_DAP_EVALUATE_EXPECT", "43")
        .env("BLYT_HOSTLUA", "1")
        .assert()
        .success();
}

/// SDL2 DAP: verify loadedSources returns the cart's Lua source after a stop.
#[test]
fn sdl_dap_loaded_sources() {
    require_sdk();
    require_lua_sdk();
    assert!(
        blytdebug().exists(),
        "blytplay not built — run `cmake --build build` first"
    );

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("sdl_dap_loaded_sources");
    CartProject::new()
        .lua(
            "function init()\n\
             \x20   local x = 42\n\
             \x20   local y = x + 1\n\
             \x20   local z = y + 1\n\
             end\n\
             function update() blyt.quit() end\n\
             function draw() end\n",
        )
        .write(&project);

    let cart = build_lua_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let orchestrator = repo_root().join("tests/dap/run_sdl_dap_test.mjs");
    Command::new("node")
        .args([
            orchestrator.to_str().unwrap(),
            blytdebug().to_str().unwrap(),
            cart.to_str().unwrap(),
        ])
        .env("BLYT_DAP_BP_LINE", "3")
        .env("BLYT_DAP_LOADED_SOURCES", "1")
        .assert()
        .success();
}

/// SDL2 DAP: conditional breakpoint — stop only when the Lua expression is true.
/// Condition references a local variable (x = 42) to verify that locals are
/// visible in condition expressions.
#[test]
fn sdl_dap_conditional_breakpoint() {
    require_sdk();
    require_lua_sdk();
    assert!(
        blytdebug().exists(),
        "blytplay not built — run `cmake --build build` first"
    );

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("sdl_dap_conditional_bp");
    CartProject::new()
        .lua(
            "function init()\n\
             \x20   local x = 42\n\
             \x20   local y = x + 1\n\
             \x20   local z = y + 1\n\
             end\n\
             function update() blyt.quit() end\n\
             function draw() end\n",
        )
        .write(&project);

    let cart = build_lua_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let orchestrator = repo_root().join("tests/dap/run_sdl_dap_test.mjs");
    Command::new("node")
        .args([
            orchestrator.to_str().unwrap(),
            blytdebug().to_str().unwrap(),
            cart.to_str().unwrap(),
        ])
        .env("BLYT_DAP_BP_LINE", "3")
        .env("BLYT_DAP_CONDITIONAL_COND", "x > 10")
        .assert()
        .success();
}

/// SDL2 DAP: restart — after the first breakpoint stop, restart the cart and
/// verify it stops at the same breakpoint a second time.
#[test]
fn sdl_dap_restart() {
    require_sdk();
    require_lua_sdk();
    assert!(
        blytdebug().exists(),
        "blytplay not built — run `cmake --build build` first"
    );

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("sdl_dap_restart");
    CartProject::new()
        .lua(
            "function init()\n\
             \x20   local x = 42\n\
             \x20   local y = x + 1\n\
             \x20   local z = y + 1\n\
             end\n\
             function update() blyt.quit() end\n\
             function draw() end\n",
        )
        .write(&project);

    let cart = build_lua_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let orchestrator = repo_root().join("tests/dap/run_sdl_dap_test.mjs");
    Command::new("node")
        .args([
            orchestrator.to_str().unwrap(),
            blytdebug().to_str().unwrap(),
            cart.to_str().unwrap(),
        ])
        .env("BLYT_DAP_BP_LINE", "3")
        .env("BLYT_DAP_TEST_RESTART", "1")
        .assert()
        .success();
}

/// SDL2 DAP: exception breakpoints — a cart that throws in init() pauses at the
/// exception and reports reason "exception" to the DAP client.
#[test]
fn sdl_dap_exception_breakpoint() {
    require_sdk();
    require_lua_sdk();
    assert!(
        blytdebug().exists(),
        "blytplay not built — run `cmake --build build` first"
    );

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("sdl_dap_exception_bp");
    CartProject::new()
        .lua(
            "function init()\n\
             \x20   error(\"test exception\")\n\
             end\n\
             function update() blyt.quit() end\n\
             function draw() end\n",
        )
        .write(&project);

    let cart = build_lua_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let orchestrator = repo_root().join("tests/dap/run_sdl_dap_test.mjs");
    Command::new("node")
        .args([
            orchestrator.to_str().unwrap(),
            blytdebug().to_str().unwrap(),
            cart.to_str().unwrap(),
        ])
        .env("BLYT_DAP_EXCEPTION_FILTER", "uncaught")
        .assert()
        .success();
}

/// SDL2 DAP: evaluate — assert that arbitrary Lua expressions are evaluated in
/// the frame context (not just variable name lookup). Stops at line 3 where
/// x = 42 is defined; evaluates "x + 1" and expects "43".
#[test]
fn sdl_dap_evaluate_expr() {
    require_sdk();
    require_lua_sdk();
    assert!(
        blytdebug().exists(),
        "blytplay not built — run `cmake --build build` first"
    );

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("sdl_dap_evaluate");
    CartProject::new()
        .lua(
            "function init()\n\
             \x20   local x = 42\n\
             \x20   local y = x + 1\n\
             \x20   local z = y + 1\n\
             end\n\
             function update() blyt.quit() end\n\
             function draw() end\n",
        )
        .write(&project);

    let cart = build_lua_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let orchestrator = repo_root().join("tests/dap/run_sdl_dap_test.mjs");
    Command::new("node")
        .args([
            orchestrator.to_str().unwrap(),
            blytdebug().to_str().unwrap(),
            cart.to_str().unwrap(),
        ])
        .env("BLYT_DAP_BP_LINE", "3")
        .env("BLYT_DAP_EVALUATE_EXPR", "x + 1")
        .env("BLYT_DAP_EVALUATE_EXPECT", "43")
        .assert()
        .success();
}

/// WASM DAP: evaluate upvalue — a module-level local is captured as an upvalue
/// of update(); evaluating it in a watch while stopped inside update() must
/// return the value (not nil).  Covers the handle_evaluate upvalue-injection fix.
#[test]
fn wasm_dap_evaluate_upvalue() {
    require_wasm_debug();
    require_lua_sdk();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("wasm_dap_evaluate_upvalue");
    // counter = 7 at module scope → captured as upvalue of update().
    // BP at line 4 (counter = counter + 1), before the increment executes.
    CartProject::new()
        .lua(
            "local counter = 7\n\
             function init() end\n\
             function update()\n\
             \x20   counter = counter + 1\n\
             \x20   blyt_quit()\n\
             end\n\
             function draw() end\n",
        )
        .write(&project);

    let cart = build_lua_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let wasm_dir = find_wasm_debug_dir();
    let orchestrator = repo_root().join("tests/dap/run_dap_test.mjs");

    Command::new("node")
        .args([
            orchestrator.to_str().unwrap(),
            wasm_dir.to_str().unwrap(),
            cart.to_str().unwrap(),
        ])
        .env("BLYT_DAP_BP_LINE", "4")
        .env("BLYT_DAP_EVALUATE_EXPR", "counter")
        .env("BLYT_DAP_EVALUATE_EXPECT", "7")
        .assert()
        .success();
}

/// SDL2 DAP: evaluate upvalue — same as wasm_dap_evaluate_upvalue but via the
/// SDL2/TCP path.
#[test]
fn sdl_dap_evaluate_upvalue() {
    require_sdk();
    require_lua_sdk();
    assert!(
        blytdebug().exists(),
        "blytplay not built — run `cmake --build build` first"
    );

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("sdl_dap_evaluate_upvalue");
    CartProject::new()
        .lua(
            "local counter = 7\n\
             function init() end\n\
             function update()\n\
             \x20   counter = counter + 1\n\
             \x20   blyt.quit()\n\
             end\n\
             function draw() end\n",
        )
        .write(&project);

    let cart = build_lua_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let orchestrator = repo_root().join("tests/dap/run_sdl_dap_test.mjs");
    Command::new("node")
        .args([
            orchestrator.to_str().unwrap(),
            blytdebug().to_str().unwrap(),
            cart.to_str().unwrap(),
        ])
        .env("BLYT_DAP_BP_LINE", "4")
        .env("BLYT_DAP_EVALUATE_EXPR", "counter")
        .env("BLYT_DAP_EVALUATE_EXPECT", "7")
        .assert()
        .success();
}

/* ── WASM DAP parity tests (features present in SDL2 but missing from WASM) ── */

/// WASM DAP: loadedSources returns the cart's Lua source after a stop.
#[test]
fn wasm_dap_loaded_sources() {
    require_wasm_debug();
    require_lua_sdk();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("wasm_dap_loaded_src");
    CartProject::new()
        .lua(
            "function init()\n\
             \x20   local x = 42\n\
             \x20   local y = x + 1\n\
             \x20   local z = y + 1\n\
             end\n\
             function update() blyt_quit() end\n\
             function draw() end\n",
        )
        .write(&project);
    let cart = build_lua_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let wasm_dir = find_wasm_debug_dir();
    let orchestrator = repo_root().join("tests/dap/run_dap_test.mjs");
    Command::new("node")
        .args([
            orchestrator.to_str().unwrap(),
            wasm_dir.to_str().unwrap(),
            cart.to_str().unwrap(),
        ])
        .env("BLYT_DAP_BP_LINE", "3")
        .env("BLYT_DAP_LOADED_SOURCES", "1")
        .assert()
        .success();
}

/// WASM DAP: restart — cart restarts and stops at the same breakpoint again.
#[test]
fn wasm_dap_restart() {
    require_wasm_debug();
    require_lua_sdk();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("wasm_dap_restart");
    CartProject::new()
        .lua(
            "function init()\n\
             \x20   local x = 42\n\
             \x20   local y = x + 1\n\
             \x20   local z = y + 1\n\
             end\n\
             function update() blyt_quit() end\n\
             function draw() end\n",
        )
        .write(&project);
    let cart = build_lua_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let wasm_dir = find_wasm_debug_dir();
    let orchestrator = repo_root().join("tests/dap/run_dap_test.mjs");
    Command::new("node")
        .args([
            orchestrator.to_str().unwrap(),
            wasm_dir.to_str().unwrap(),
            cart.to_str().unwrap(),
        ])
        .env("BLYT_DAP_BP_LINE", "3")
        .env("BLYT_DAP_TEST_RESTART", "1")
        .assert()
        .success();
}

/// WASM DAP: exception breakpoint — cart throws in init(), DAP stops at the error.
#[test]
fn wasm_dap_exception_breakpoint() {
    require_wasm_debug();
    require_lua_sdk();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("wasm_dap_exception");
    CartProject::new()
        .lua(
            "function init()\n\
             \x20   error(\"test exception\")\n\
             end\n\
             function update() blyt_quit() end\n\
             function draw() end\n",
        )
        .write(&project);
    let cart = build_lua_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let wasm_dir = find_wasm_debug_dir();
    let orchestrator = repo_root().join("tests/dap/run_dap_test.mjs");
    Command::new("node")
        .args([
            orchestrator.to_str().unwrap(),
            wasm_dir.to_str().unwrap(),
            cart.to_str().unwrap(),
        ])
        .env("BLYT_DAP_EXCEPTION_FILTER", "uncaught")
        .assert()
        .success();
}

/// WASM DAP: a Lua error during init() with NO exception breakpoint must be
/// reported and exit the debug runtime cleanly — not abort at runtimeKeepalivePop
/// (issue #102).  The buggy error path called both emscripten_cancel_main_loop()
/// and emscripten_force_exit(), which double-counts the runtime keepalive and
/// aborts the ASSERTIONS-on debug runtime.  Here we drive a normal session
/// (configurationDone, no filter), let the cart error in init(), and assert the
/// runtime reported the Lua error, did not abort, and the orchestrator exited 0.
#[test]
fn wasm_dap_init_error_reports_cleanly() {
    require_wasm_debug();
    require_lua_sdk();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("wasm_dap_init_error");
    CartProject::new()
        .lua(
            "function init()\n\
             \x20   nonexistent_global.field = 1\n\
             end\n\
             function update() blyt_quit() end\n\
             function draw() end\n",
        )
        .write(&project);
    let cart = build_lua_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let wasm_dir = find_wasm_debug_dir();
    let orchestrator = repo_root().join("tests/dap/run_dap_test.mjs");
    let output = Command::new("node")
        .args([
            orchestrator.to_str().unwrap(),
            wasm_dir.to_str().unwrap(),
            cart.to_str().unwrap(),
        ])
        .env("BLYT_DAP_EXPECT_INIT_ERROR", "1")
        .assert()
        .success()
        .get_output()
        .clone();

    let combined = format!(
        "{}{}",
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
    assert!(
        combined.contains("attempt to index a nil value"),
        "expected the Lua init error to be reported, got:\n{combined}"
    );
    assert!(
        !combined.contains("Aborted") && !combined.contains("Assertion failed"),
        "debug runtime aborted instead of reporting the Lua error cleanly:\n{combined}"
    );
}

/// SDL2 DAP: Lua breakpoint in a hybrid (Lua+C) cart — DAP only, no GDB.
///
/// Builds a Lua+C hybrid cart (has both src/game/lua/ and src/game/c/) and
/// verifies that Lua breakpoints still fire when running under blytdebug
/// with only the DAP debug server active.  This isolates whether the Lua DAP
/// hook works correctly in hybrid carts independent of GDB interaction.
#[test]
fn sdl_hybrid_lua_c_dap_no_gdb() {
    require_sdk();
    require_lua_sdk();
    assert!(
        blytdebug().exists(),
        "blytdebug not built — run `cmake --build build` first"
    );

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("hybrid_lua_c_no_gdb");
    CartProject::new()
        .lua(
            "function init()\n\
             \x20   local x = 42\n\
             \x20   local y = x + 1\n\
             \x20   local z = y + 1\n\
             end\n\
             function update() blyt.quit() end\n\
             function draw() end\n",
        )
        .c("#include \"blyt.h\"\n\
             void c_helper(void) { (void)0; }\n")
        .write(&project);

    let cart = build_debug_lua_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let orchestrator = repo_root().join("tests/dap/run_sdl_dap_test.mjs");
    Command::new("node")
        .args([
            orchestrator.to_str().unwrap(),
            blytdebug().to_str().unwrap(),
            cart.to_str().unwrap(),
        ])
        .env("BLYT_DAP_BP_LINE", "3")
        .assert()
        .success();
}

/// WASM DAP: Lua breakpoint in a hybrid (Lua+C) cart — DAP only, no GDB.
///
/// WASM equivalent of sdl_hybrid_lua_c_dap_no_gdb.  The WASM debug runtime has
/// both DAP and GDB compiled in, but when no GDB client connects the runtime
/// simply runs without GDB; Lua breakpoints must still fire correctly.
///
/// The DAP+GDB WASM combination (blyt_native_work both paused by DAP and
/// stepped under GDB) is covered by wasm_hybrid_gdb_and_dap in gdb.rs.
#[test]
fn wasm_hybrid_lua_c_dap_no_gdb() {
    require_sdk();
    require_lua_sdk();
    require_wasm_debug();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("wasm_hybrid_lua_c_no_gdb");
    CartProject::new()
        .lua(
            "function init()\n\
             \x20   local x = 42\n\
             \x20   local y = x + 1\n\
             \x20   local z = y + 1\n\
             end\n\
             function update() blyt_quit() end\n\
             function draw() end\n",
        )
        .c("#include \"blyt.h\"\n\
             void c_helper(void) { (void)0; }\n")
        .write(&project);

    let cart = build_debug_lua_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let wasm_dir = find_wasm_debug_dir();
    let orchestrator = repo_root().join("tests/dap/run_dap_test.mjs");
    Command::new("node")
        .args([
            orchestrator.to_str().unwrap(),
            wasm_dir.to_str().unwrap(),
            cart.to_str().unwrap(),
        ])
        .env("BLYT_DAP_BP_LINE", "3")
        .assert()
        .success();
}

/// SDL2 DAP: Lua breakpoint in a hybrid (Lua+C) cart — full hybrid mode.
///
/// Builds a Lua+C hybrid debug cart and runs blytdebug with both DAP and GDB
/// active (--debug 0 --gdb 0 --headless).  A minimal GDB RSP stub connects to
/// the GDB port to unblock blyt_session_gdb_wait_attached; dap_test.mjs
/// connects to the Lua DAP port and verifies the Lua breakpoint fires.
///
/// This is the exact scenario reported as failing: Lua BPs don't stop in
/// hybrid mode while C BPs (via lldb-dap) do.
#[test]
fn sdl_hybrid_lua_c_dap_with_gdb() {
    require_sdk();
    require_lua_sdk();
    require_gdb();
    assert!(
        blytdebug().exists(),
        "blytdebug not built — run `cmake --build build` first"
    );

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("hybrid_lua_c_with_gdb");
    CartProject::new()
        .lua(
            "function init()\n\
             \x20   local x = 42\n\
             \x20   local y = x + 1\n\
             \x20   local z = y + 1\n\
             end\n\
             function update() blyt.quit() end\n\
             function draw() end\n",
        )
        .c("#include \"blyt.h\"\n\
             void c_helper(void) { (void)0; }\n")
        .write(&project);

    let cart = build_debug_lua_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let orchestrator = repo_root().join("tests/dap/run_hybrid_dap_test.mjs");
    Command::new("node")
        .args([
            orchestrator.to_str().unwrap(),
            blytdebug().to_str().unwrap(),
            cart.to_str().unwrap(),
        ])
        .env("BLYT_DAP_BP_LINE", "3")
        .assert()
        .success();
}

/// A cart with a nested call so the stack has more than one Lua frame:
/// `init()` calls `deep()`, and the breakpoint sits on line 2 inside `deep()`.
/// At the stop the call stack is deep → init → main chunk, all in main.lua.
/// `update`'s quit call differs per leg (blyt.quit vs blyt_quit).
fn nested_call_cart(quit_call: &str) -> String {
    format!(
        "function deep()\n\
         \x20   local a = 1\n\
         end\n\
         function init()\n\
         \x20   deep()\n\
         end\n\
         function update() {quit_call} end\n\
         function draw() end\n"
    )
}

/// SDL2 DAP source-map (issue #51): with a sourceMap in the launch request the
/// relay reverse-maps the workspace breakpoint path inward (so it binds via
/// exact, not basename, matching) and localises every stackTrace frame outward
/// to the workspace — including non-top frames, which the #47 stop-reveal alone
/// did not fix.  run_sdl_dap_test.mjs in localize mode drives both assertions.
#[test]
fn sdl_dap_source_map_localizes_frames() {
    require_sdk();
    require_lua_sdk();
    assert!(
        blytdebug().exists(),
        "blytplay not built — run `cmake --build build` first"
    );

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("sdl_dap_source_map");
    CartProject::new()
        .lua(&nested_call_cart("blyt.quit()"))
        .write(&project);

    let cart = build_lua_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let orchestrator = repo_root().join("tests/dap/run_sdl_dap_test.mjs");
    Command::new("node")
        .args([
            orchestrator.to_str().unwrap(),
            blytdebug().to_str().unwrap(),
            cart.to_str().unwrap(),
        ])
        .env("BLYT_DAP_BP_LINE", "2")
        .env("BLYT_DAP_LOCALIZE", "1")
        .assert()
        .success();
}

/// WASM DAP source-map (issue #51): WASM-relay equivalent of
/// sdl_dap_source_map_localizes_frames — the relay localises every frame and
/// binds the workspace breakpoint path via the launch sourceMap.
#[test]
fn wasm_dap_source_map_localizes_frames() {
    require_wasm_debug();
    require_lua_sdk();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("wasm_dap_source_map");
    CartProject::new()
        .lua(&nested_call_cart("blyt_quit()"))
        .write(&project);

    let cart = build_lua_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let wasm_dir = find_wasm_debug_dir();
    let orchestrator = repo_root().join("tests/dap/run_dap_test.mjs");
    Command::new("node")
        .args([
            orchestrator.to_str().unwrap(),
            wasm_dir.to_str().unwrap(),
            cart.to_str().unwrap(),
        ])
        .env("BLYT_DAP_BP_LINE", "2")
        .env("BLYT_DAP_LOCALIZE", "1")
        .assert()
        .success();
}

/// Multi-file Lua cart: breakpoint in a non-first file binds and stops.
///
/// `aaa.lua` sorts before `main.lua`; previously both were compiled into one
/// chunk named after `aaa.lua` so a breakpoint in `main.lua` never bound.
/// With per-file chunks each source keeps its own name and line numbers (#54).
#[test]
fn sdl_dap_multifile_nonfirst_file_breakpoint_binds() {
    require_sdk();
    require_lua_sdk();
    assert!(
        blytdebug().exists(),
        "blytplay not built — run `cmake --build build` first"
    );

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("sdl_dap_multifile");
    // aaa.lua sorts before main.lua, so it becomes the single chunk's name.
    // The breakpoint at main.lua:2 must bind despite living in the later file.
    // inner() calls helper() at line 2 so the call stack has two frames when
    // the breakpoint fires: inner (main.lua:2) and init (main.lua:5).
    CartProject::new()
        .lua_file("aaa.lua", "function helper()\n    return 1\nend\n")
        .lua(
            "local function inner()\n\
             \x20   local x = helper()\n\
             \x20   blyt.quit()\n\
             end\n\
             function init() inner() end\n\
             function update() end\n\
             function draw() end\n",
        )
        .write(&project);

    let cart = build_lua_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let orchestrator = repo_root().join("tests/dap/run_sdl_dap_test.mjs");
    Command::new("node")
        .args([
            orchestrator.to_str().unwrap(),
            blytdebug().to_str().unwrap(),
            cart.to_str().unwrap(),
        ])
        .env("BLYT_DAP_BP_LINE", "2")
        .env("BLYT_DAP_LOCALIZE", "1")
        .assert()
        .success();
}

/// WASM-dev reload-while-debugging (issue #90, slice 4): a hot reload in a debug
/// session must be *uninterrupted* — the same DAP connection keeps working
/// across the cart swap (ADR-0045 "DAP server continuity").  Builds two cart
/// versions sharing a layout (breakpoint at the draw() line, which runs every
/// frame), stops at it, clears + continues, hot-reloads v1→v2 straight into the
/// runtime, then re-arms the breakpoint and asserts the reloaded cart stops
/// again on the same connection.  reload_dap_test.mjs returns non-zero if the
/// session was torn down (the re-armed setBreakpoints / stopped would time out).
#[test]
fn wasm_dap_reload_keeps_session() {
    require_wasm_debug();
    require_lua_sdk();

    const RELOAD_DAP_CONFIG: &str = "\
save_version: 5
records:
  Game:
    fields:
      - { name: score, type: i32 }
state_buffers:
  game:
    record: Game
    count: 1
";

    // v1/v2 share line numbers so the breakpoint at the draw() line (8) binds in
    // both; only init()'s score differs, so a state-preserving reload is
    // observable.  on_load_state prints the HOT_RELOAD reason the driver checks.
    let cart_lua = |tag: &str, score: i32| {
        format!(
            "local slot = -1\n\
             function init()\n\
             \x20   slot = blyt.buf.alloc_slot(S.GAME)\n\
             \x20   S.game[slot].score = {score}\n\
             end\n\
             function update() end\n\
             function draw()\n\
             \x20   local s = S.game[slot].score\n\
             end\n\
             function on_load_state(info)\n\
             \x20   blyt.debug.print('{tag} load reason=' .. tostring(info.reason))\n\
             end\n"
        )
    };

    let tmp = TempDir::new().unwrap();
    let p1 = tmp.path().join("reload_dap_v1");
    CartProject::new()
        .config(RELOAD_DAP_CONFIG)
        .lua(&cart_lua("v1", 7))
        .write(&p1);
    let cart_v1 = build_lua_cart(&p1);

    let p2 = tmp.path().join("reload_dap_v2");
    CartProject::new()
        .config(RELOAD_DAP_CONFIG)
        .lua(&cart_lua("v2", 100))
        .write(&p2);
    let cart_v2 = build_lua_cart(&p2);

    let wasm_dir = find_wasm_debug_dir();
    let orchestrator = repo_root().join("tests/dap/reload_dap_test.mjs");
    Command::new("node")
        .args([
            orchestrator.to_str().unwrap(),
            wasm_dir.to_str().unwrap(),
            cart_v1.to_str().unwrap(),
            cart_v2.to_str().unwrap(),
            "8",
        ])
        .assert()
        .success();
}

/// SDL2 Lua DAP reload continuity (issue #140): a hot reload on a pure-Lua
/// native-window debug session must be *uninterrupted* — the same DAP TCP
/// connection keeps working, and source-line breakpoints re-arm on the new
/// Lua state without a session restart.  Builds two cart versions sharing
/// the same init() line layout (breakpoint at line 2), stops at it, continues,
/// fires a dev-control reload to swap in v2, then asserts the same connection
/// delivers a second init() stop (re-arm confirmed, session persisted, no
/// restart needed).
#[test]
fn sdl_dap_lua_reload_keeps_session() {
    require_sdk();
    require_lua_sdk();
    assert!(
        blytdebug().exists(),
        "blytdebug not built — run `cmake --build build` first"
    );

    // Both carts share the same line structure so the breakpoint at line 2
    // (the `local x = ...` assignment) is valid in both.  The value of x
    // differs between v1/v2 so the builds are byte-distinct.
    let cart_lua = |val: i32| {
        format!(
            "function init()\n\
             \x20   local x = {val}\n\
             end\n\
             function update() end\n\
             function draw() end\n"
        )
    };

    let tmp = TempDir::new().unwrap();
    let p1 = tmp.path().join("sdl_lua_reload_v1");
    CartProject::new().lua(&cart_lua(42)).write(&p1);
    let cart_v1 = build_lua_cart(&p1);

    let p2 = tmp.path().join("sdl_lua_reload_v2");
    CartProject::new().lua(&cart_lua(99)).write(&p2);
    let cart_v2 = build_lua_cart(&p2);

    let orchestrator = repo_root().join("tests/dap/run_sdl_lua_reload_test.mjs");
    Command::new("node")
        .args([
            orchestrator.to_str().unwrap(),
            blytdebug().to_str().unwrap(),
            cart_v1.to_str().unwrap(),
            cart_v2.to_str().unwrap(),
            "2",
        ])
        .assert()
        .success();
}
