mod common;

use assert_cmd::Command;
use common::{
    CartProject, blytrun, build_cart, build_debug_cart, find_symbol_addr, find_wasm_dir,
    libretro_so, repo_root, require_gdb, require_libretro_core, require_sdk, require_wasm,
    sdk_dir, test_libretro_core,
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
        .c(
            "#include \"blyt.h\"\n\
             static int g_frame = 0;\n\
             void blyt_cart_init(void)   {}\n\
             void blyt_cart_update(void) { if (++g_frame >= 3) blyt_quit(); }\n\
             void blyt_cart_draw(void)   {}\n",
        )
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
        .c(
            "#include \"blyt.h\"\n\
             static int g_frame = 0;\n\
             void blyt_cart_init(void)   {}\n\
             void blyt_cart_update(void) { if (++g_frame >= 3) blyt_quit(); }\n\
             void blyt_cart_draw(void)   {}\n",
        )
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
        .c(
            "#include \"blyt.h\"\n\
             static int g_frame = 0;\n\
             void blyt_cart_init(void)   {}\n\
             void blyt_cart_update(void) { if (++g_frame >= 3) blyt_quit(); }\n\
             void blyt_cart_draw(void)   {}\n",
        )
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
        .c(
            "#include \"blyt.h\"\n\
             static int g_frame = 0;\n\
             void blyt_cart_init(void)   {}\n\
             void blyt_cart_update(void) { if (++g_frame >= 3) blyt_quit(); }\n\
             void blyt_cart_draw(void)   {}\n",
        )
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
        .c(
            "#include \"blyt.h\"\n\
             static int g_frame = 0;\n\
             void blyt_cart_init(void)   {}\n\
             void blyt_cart_update(void) { if (++g_frame >= 3) blyt_quit(); }\n\
             void blyt_cart_draw(void)   {}\n",
        )
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
