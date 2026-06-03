mod common;

use assert_cmd::Command;
use common::{
    CartProject, blytdebug, build_lua_cart, debug_lib_dir, find_wasm_dir, repo_root,
    require_lua_sdk, require_sdk, require_wasm,
};
use tempfile::TempDir;

/// WASM DAP gate test: set a breakpoint in a Lua cart running inside blytplay.js,
/// verify the stopped event, inspect locals, step over, and continue to completion.
///
/// Requires the WASM runtime to have been built with BLYT_DAP=ON:
///   emcmake cmake -B build-wasm -S frontends/wasm -DBLYT_DAP=ON
///   cmake --build build-wasm
///
/// Silently asserts on missing WASM runtime or Lua SDK.
#[test]
fn wasm_dap_breakpoint_step_inspect() {
    require_wasm();
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

    let wasm_dir = find_wasm_dir();
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
    require_wasm();
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

    let wasm_dir = find_wasm_dir();
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
    require_wasm();
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

    let wasm_dir = find_wasm_dir();
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
    require_wasm();
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

    let wasm_dir = find_wasm_dir();
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
        .env("BLYT_LIB_DIR", debug_lib_dir())
        .env("BLYT_DAP_BP_LINE", "3")
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
        .env("BLYT_LIB_DIR", debug_lib_dir())
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
        .env("BLYT_LIB_DIR", debug_lib_dir())
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
        .env("BLYT_LIB_DIR", debug_lib_dir())
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
        .env("BLYT_LIB_DIR", debug_lib_dir())
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
        .env("BLYT_LIB_DIR", debug_lib_dir())
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
    require_wasm();
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

    let wasm_dir = find_wasm_dir();
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
        .env("BLYT_LIB_DIR", debug_lib_dir())
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
    require_wasm();
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

    let wasm_dir = find_wasm_dir();
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
    require_wasm();
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

    let wasm_dir = find_wasm_dir();
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
    require_wasm();
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

    let wasm_dir = find_wasm_dir();
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
