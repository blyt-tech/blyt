mod common;

use assert_cmd::Command;
use common::{
    CartProject, blytrun, build_cart, build_debug_cart, build_debug_lua_cart, find_symbol_addr,
    find_wasm_dir, libretro_so, repo_root, require_gdb, require_libretro_core, require_lua_sdk,
    require_sdk, require_wasm, sdk_dir, test_libretro_core,
};
use tempfile::TempDir;

/// SDL2 GDB handshake: connect to a C cart's GDB server and verify the RSP
/// protocol works end-to-end (qSupported, vCont;c, session complete).
///
/// Does not set a breakpoint — verifies transport and handshake only.
///
/// Requires: blytrun built with BLYT_GDB=ON, SDK assembled.
#[test]
fn sdl_gdb_handshake() {
    require_sdk();
    require_gdb();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("sdl_gdb_c");
    CartProject::new()
        .c("#include \"blyt.h\"\n\
             static int g_frame = 0;\n\
             void blyt_cart_init(void)   {}\n\
             void blyt_cart_update(void) { if (++g_frame >= 3) blyt_quit(); }\n\
             void blyt_cart_draw(void)   {}\n")
        .write(&project);

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let orchestrator = repo_root().join("tests/gdb/run_sdl_gdb_test.mjs");
    Command::new("node")
        .args([
            orchestrator.to_str().unwrap(),
            blytrun().to_str().unwrap(),
            cart.to_str().unwrap(),
        ])
        .env("BLYT_LIB_DIR", sdk_dir().join("lib"))
        .assert()
        .success();
}

/// SDL2 GDB breakpoint + step: build a C cart with debug info, set a Z0
/// software breakpoint at `blyt_cart_init`, verify T05 stop reply and register
/// read, single-step, then continue to completion.
///
/// Requires: blytrun with BLYT_GDB=ON, SDK, `readelf` on PATH.
#[test]
fn sdl_gdb_breakpoint_step() {
    require_sdk();
    require_gdb();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("sdl_gdb_bp");
    CartProject::new()
        .c("#include \"blyt.h\"\n\
             static int g_frame = 0;\n\
             void blyt_cart_init(void)   {}\n\
             void blyt_cart_update(void) { if (++g_frame >= 3) blyt_quit(); }\n\
             void blyt_cart_draw(void)   {}\n")
        .write(&project);

    let cart = build_debug_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let addr = find_symbol_addr(&cart, "blyt_cart_init");
    if addr.is_none() {
        // readelf not available or symbol stripped — fall back to handshake only.
        let orchestrator = repo_root().join("tests/gdb/run_sdl_gdb_test.mjs");
        Command::new("node")
            .args([
                orchestrator.to_str().unwrap(),
                blytrun().to_str().unwrap(),
                cart.to_str().unwrap(),
            ])
            .env("BLYT_LIB_DIR", sdk_dir().join("lib"))
            .assert()
            .success();
        return;
    }
    let addr = addr.unwrap();

    let orchestrator = repo_root().join("tests/gdb/run_sdl_gdb_test.mjs");
    Command::new("node")
        .args([
            orchestrator.to_str().unwrap(),
            blytrun().to_str().unwrap(),
            cart.to_str().unwrap(),
        ])
        .env("BLYT_LIB_DIR", sdk_dir().join("lib"))
        .env("BLYT_GDB_BREAK_ADDR", format!("{addr:x}"))
        .assert()
        .success();
}

/// SDL2 GDB: Rust cart — same breakpoint+step flow with a Rust source cart.
///
/// The breakpoint is placed at the entry to `blyt_cart_init` (linked from the
/// Rust `#[no_mangle] extern "C" fn blyt_cart_init()`).
#[test]
fn sdl_gdb_rust_cart() {
    use common::require_rust_riscv_target;
    require_sdk();
    require_gdb();
    require_rust_riscv_target();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("sdl_gdb_rust");
    CartProject::new()
        .rust(
            "#![no_std]\n\
             static mut G_FRAME: u32 = 0;\n\
             #[no_mangle]\n\
             pub extern \"C\" fn blyt_cart_init() {}\n\
             #[no_mangle]\n\
             pub extern \"C\" fn blyt_cart_update() {\n\
             \x20   unsafe {\n\
             \x20       G_FRAME += 1;\n\
             \x20       if G_FRAME >= 3 { blyt::quit(); }\n\
             \x20   }\n\
             }\n\
             #[no_mangle]\n\
             pub extern \"C\" fn blyt_cart_draw() {}\n",
        )
        .write(&project);

    let cart = build_debug_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    // Find the function address; fall back to plain handshake if readelf is absent.
    let addr = find_symbol_addr(&cart, "blyt_cart_init");
    let orchestrator = repo_root().join("tests/gdb/run_sdl_gdb_test.mjs");
    let mut cmd = Command::new("node");
    cmd.args([
        orchestrator.to_str().unwrap(),
        blytrun().to_str().unwrap(),
        cart.to_str().unwrap(),
    ])
    .env("BLYT_LIB_DIR", sdk_dir().join("lib"));
    if let Some(a) = addr {
        cmd.env("BLYT_GDB_BREAK_ADDR", format!("{a:x}"));
    }
    cmd.assert().success();
}

/// Libretro GDB port pickup: load a C cart via the libretro API with
/// BLYT_GDB_PORT=0, verify the GDB server starts and the cart runs to
/// completion without a connected client.
///
/// This tests the `BLYT_GDB_PORT` env-var code path in `blyt_libretro.c`
/// without requiring a GDB client (the cart quits after 3 frames regardless).
///
/// Requires: blyt_libretro.so built with BLYT_GDB=ON, test_libretro_core binary.
#[test]
fn libretro_gdb_listen_and_handshake() {
    require_sdk();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("libretro_gdb_c");
    CartProject::new()
        .c("#include \"blyt.h\"\n\
             static int g_frame = 0;\n\
             void blyt_cart_init(void)   {}\n\
             void blyt_cart_update(void) { if (++g_frame >= 3) blyt_quit(); }\n\
             void blyt_cart_draw(void)   {}\n")
        .write(&project);

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    // Run the cart through test_libretro_core with BLYT_GDB_PORT=0 set.
    // The GDB server will start on an OS-assigned port.  No client connects;
    // the cart runs to completion (3 frames) and the test exits cleanly.
    Command::new(test_libretro_core())
        .args([libretro_so().to_str().unwrap(), cart.to_str().unwrap()])
        .env("BLYT_GDB_PORT", "0")
        .assert()
        .success();
}

/// WASM GDB handshake: load a C cart in blytrun.js with the GDB relay port
/// injected, connect gdb_test.mjs via WebSocket, and verify the session
/// completes successfully.
///
/// Requires: WASM build with BLYT_GDB=ON.
#[test]
fn wasm_gdb_handshake() {
    require_sdk();
    require_wasm();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("wasm_gdb_c");
    CartProject::new()
        .c("#include \"blyt.h\"\n\
             static int g_frame = 0;\n\
             void blyt_cart_init(void)   {}\n\
             void blyt_cart_update(void) { if (++g_frame >= 3) blyt_quit(); }\n\
             void blyt_cart_draw(void)   {}\n")
        .write(&project);

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let orchestrator = repo_root().join("tests/gdb/run_gdb_test.mjs");
    Command::new("node")
        .args([
            orchestrator.to_str().unwrap(),
            find_wasm_dir().to_str().unwrap(),
            cart.to_str().unwrap(),
        ])
        .assert()
        .success();
}

/// WASM GDB breakpoint + step: C cart in WASM, breakpoint at blyt_cart_init.
///
/// Requires: WASM build with BLYT_GDB=ON, `readelf` on PATH.
#[test]
fn wasm_gdb_breakpoint_step() {
    require_sdk();
    require_wasm();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("wasm_gdb_bp");
    CartProject::new()
        .c("#include \"blyt.h\"\n\
             static int g_frame = 0;\n\
             void blyt_cart_init(void)   {}\n\
             void blyt_cart_update(void) { if (++g_frame >= 3) blyt_quit(); }\n\
             void blyt_cart_draw(void)   {}\n")
        .write(&project);

    let cart = build_debug_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let addr = find_symbol_addr(&cart, "blyt_cart_init");
    let orchestrator = repo_root().join("tests/gdb/run_gdb_test.mjs");
    let mut cmd = Command::new("node");
    cmd.args([
        orchestrator.to_str().unwrap(),
        find_wasm_dir().to_str().unwrap(),
        cart.to_str().unwrap(),
    ]);
    if let Some(a) = addr {
        cmd.env("BLYT_GDB_BREAK_ADDR", format!("{a:x}"));
    }
    cmd.assert().success();
}

/// SDL2 hybrid: Lua cart with a C native library — both DAP and GDB active
/// simultaneously.
///
/// Scenario:
///   - DAP breakpoint at line 3 (Lua call site: `blyt_native_work()`)
///   - GDB software breakpoint at the C function entry
///   - DAP "next" triggers the native call → GDB fires inside C
///   - GDB single-steps × 2, then continues
///   - DAP receives stopped at line 4 (step-over complete)
///
/// WASM hybrid is architecturally unsupported: Lua carts on WASM bypass
/// rv32emu (host-side Lua interpreter), so GDB (which operates on the
/// rv32emu PC) cannot intercept native C calls made from Lua.  The SDL2 /
/// libretro paths run the Lua interpreter inside rv32emu, so both debuggers
/// operate on the same execution stream.
///
/// Requires: blytrun built with BLYT_DAP=ON and BLYT_GDB=ON, Lua SDK,
/// `readelf` on PATH (for symbol address; falls back to DAP-only if absent).
#[test]
fn sdl_hybrid_gdb_and_dap() {
    require_sdk();
    require_lua_sdk();
    require_gdb();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("hybrid_gdb_dap");

    // C library: blyt_native_work is a Lua C function with a couple of
    // volatile assignments so that two single-steps land on distinct PCs.
    const C_SOURCE: &str = r#"
#include "lua.h"
#include "lauxlib.h"

int blyt_native_work(lua_State *L) {
    volatile int x = 42;
    volatile int y = x + 1;
    (void)y;
    return 0;
}

void cart_lua_modules(lua_State *L) {
    lua_pushcfunction(L, blyt_native_work);
    lua_setglobal(L, "blyt_native_work");
}
"#;

    // Lua cart: line 3 is the native call site; line 4 is the expected DAP
    // landing point after the step-over completes.
    const LUA_SOURCE: &str = "\
function init()\n\
    local _ = 0\n\
    blyt_native_work()\n\
    local done = true\n\
    blyt.quit()\n\
end\n\
\n\
function update() end\n\
function draw()   end\n";

    CartProject::new()
        .lib_file("nativework", "nativework.c", C_SOURCE)
        .lua(LUA_SOURCE)
        .write(&project);

    let cart = build_debug_lua_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    // Look up blyt_native_work in the cart ELF symbol table.  With --debug
    // the symbol is in .symtab even though it is statically linked in.
    let addr = find_symbol_addr(&cart, "blyt_native_work");

    let orchestrator = repo_root().join("tests/gdb/run_sdl_hybrid_test.mjs");
    let mut cmd = Command::new("node");
    cmd.args([
        orchestrator.to_str().unwrap(),
        blytrun().to_str().unwrap(),
        cart.to_str().unwrap(),
    ])
    .env("BLYT_LIB_DIR", sdk_dir().join("lib"));

    if let Some(a) = addr {
        cmd.env("BLYT_GDB_BREAK_ADDR", format!("{a:x}"));
    }

    cmd.assert().success();
}
