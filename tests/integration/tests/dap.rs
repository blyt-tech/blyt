mod common;

use assert_cmd::Command;
use common::{
    CartProject, blytrun, build_cart, build_lua_cart, find_wasm_dir, libretro_so, repo_root,
    require_libretro_dap, require_lua_sdk, require_sdk, require_wasm, sdk_dir, test_libretro_dap,
    write_c_cart_project,
};
use tempfile::TempDir;

/// WASM DAP gate test: set a breakpoint in a Lua cart running inside blytrun.js,
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

/// SDL2 DAP gate: set a breakpoint in a Lua cart running under blytrun --debug
/// --headless, connect a TCP DAP client, verify stopped / step / continue.
///
/// Requires the native build (blytrun + libblyt32lua.so) and Lua SDK.
/// Uses BLYT_DAP_BP_LINE=3 (third line of init(), "local y = x + 1").
#[test]
fn sdl_dap_breakpoint_step_inspect() {
    require_sdk();
    require_lua_sdk();
    assert!(
        blytrun().exists(),
        "blytrun not built — run `cmake --build build` first"
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
            blytrun().to_str().unwrap(),
            cart.to_str().unwrap(),
        ])
        .env("BLYT_LIB_DIR", sdk_dir().join("lib"))
        .env("BLYT_DAP_BP_LINE", "3")
        .assert()
        .success();
}

/// Libretro DAP handshake: load a cart via the libretro API with BLYT_DAP_PORT=0,
/// connect a TCP socket, and verify the DAP server responds to "initialize".
///
/// Covers: blyt_session_dap_listen, BLYT_DAP_PORT env-var pickup, TCP server
/// startup, and the DAP initialize handshake on the emulated-cart path.
///
/// Requires: blyt_libretro.so and test_libretro_dap binary (RV32 toolchain).
#[test]
fn libretro_dap_listen_and_handshake() {
    require_sdk();
    require_libretro_dap();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("libretro_dap_c");
    write_c_cart_project(
        &project,
        "#include \"blyt.h\"\n\
         void blyt_cart_init(void)   {}\n\
         void blyt_cart_update(void) {}\n\
         void blyt_cart_draw(void)   {}\n",
    );
    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    Command::new(test_libretro_dap())
        .args([libretro_so().to_str().unwrap(), cart.to_str().unwrap()])
        .assert()
        .success();
}
