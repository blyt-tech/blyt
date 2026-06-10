mod common;

use assert_cmd::Command;
use common::{
    CartProject, blytdebug, build_cart, build_debug_cart, build_debug_lua_cart,
    find_wasm_debug_dir, repo_root, require_gdb, require_lua_sdk, require_rust_riscv_target,
    require_sdk, require_symbol_addr, require_wasm_debug,
};
use tempfile::TempDir;

/// SDL2 GDB handshake: connect to a C cart's GDB server and verify the RSP
/// protocol works end-to-end (qSupported, vCont;c, session complete).
///
/// Does not set a breakpoint — verifies transport and handshake only.
///
/// Requires: blytplay built with BLYT_GDB=ON, SDK assembled.
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
            blytdebug().to_str().unwrap(),
            cart.to_str().unwrap(),
        ])
        .assert()
        .success();
}

/// SDL2 GDB breakpoint + step: build a C cart with debug info, set a Z0
/// software breakpoint at `blyt_cart_init`, verify T05 stop reply and register
/// read, single-step, then continue to completion.
///
/// Requires: blytplay with BLYT_GDB=ON, SDK, `readelf` on PATH.
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

    let addr = require_symbol_addr(&cart, "blyt_cart_init");

    let orchestrator = repo_root().join("tests/gdb/run_sdl_gdb_test.mjs");
    Command::new("node")
        .args([
            orchestrator.to_str().unwrap(),
            blytdebug().to_str().unwrap(),
            cart.to_str().unwrap(),
        ])
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

    let addr = require_symbol_addr(&cart, "blyt_cart_init");
    let orchestrator = repo_root().join("tests/gdb/run_sdl_gdb_test.mjs");
    let mut cmd = Command::new("node");
    cmd.args([
        orchestrator.to_str().unwrap(),
        blytdebug().to_str().unwrap(),
        cart.to_str().unwrap(),
    ]);
    cmd.env("BLYT_GDB_BREAK_ADDR", format!("{addr:x}"));
    cmd.assert().success();
}

/// WASM GDB: Rust cart — same breakpoint+step flow as sdl_gdb_rust_cart on the
/// WASM path.
#[test]
fn wasm_gdb_rust_cart() {
    require_sdk();
    require_wasm_debug();
    require_rust_riscv_target();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("wasm_gdb_rust");
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

    let addr = require_symbol_addr(&cart, "blyt_cart_init");
    let orchestrator = repo_root().join("tests/gdb/run_gdb_test.mjs");
    let mut cmd = Command::new("node");
    cmd.args([
        orchestrator.to_str().unwrap(),
        find_wasm_debug_dir().to_str().unwrap(),
        cart.to_str().unwrap(),
    ]);
    cmd.env("BLYT_GDB_BREAK_ADDR", format!("{addr:x}"));
    cmd.assert().success();
}

/// WASM GDB handshake: load a C cart in blytplay.js with the GDB relay port
/// injected, connect gdb_test.mjs via WebSocket, and verify the session
/// completes successfully.
///
/// Requires: WASM build with BLYT_GDB=ON.
#[test]
fn wasm_gdb_handshake() {
    require_sdk();
    require_wasm_debug();

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
            find_wasm_debug_dir().to_str().unwrap(),
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
    require_wasm_debug();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("wasm_gdb_bp");
    CartProject::new()
        .c("#include \"blyt.h\"\n\
             static int g_frame = 0;\n\
             void blyt_cart_init(void)   {}\n\
             void blyt_cart_update(void) { if (++g_frame >= 3) blyt_quit(); }\n\
             void blyt_cart_draw(void)   {}\n")
        .write(&project);

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let addr = require_symbol_addr(&cart, "blyt_cart_init");
    let orchestrator = repo_root().join("tests/gdb/run_gdb_test.mjs");
    let mut cmd = Command::new("node");
    cmd.args([
        orchestrator.to_str().unwrap(),
        find_wasm_debug_dir().to_str().unwrap(),
        cart.to_str().unwrap(),
    ]);
    cmd.env("BLYT_GDB_BREAK_ADDR", format!("{addr:x}"));
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
/// For the equivalent test on the WASM path (host-side Lua + rv32emu session
/// for native calls), see wasm_hybrid_gdb_and_dap.
///
/// Requires: blytplay built with BLYT_DAP=ON and BLYT_GDB=ON, Lua SDK,
/// `readelf` on PATH (for symbol address; falls back to DAP-only if absent).
#[test]
fn sdl_hybrid_gdb_and_dap() {
    require_sdk();
    require_lua_sdk();
    require_gdb();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("hybrid_gdb_dap");

    // C game code: blyt_native_work exported via BLYT_LUA_EXPORT_VOID so the macro
    // emits both the .lua_regtab registration (SDL2/libretro) and the .lua_exports
    // metadata (WASM trampoline).  Uses .c() so the code ends up in src/game/c/,
    // which (a) is always linked as a direct object and (b) triggers glue-file
    // generation (native_count > 0) so cart_lua_modules iterates .lua_regtab.
    // Volatile assignments give GDB two distinct PCs.
    const C_SOURCE: &str = r#"
#include "blyt.h"

BLYT_LUA_EXPORT_VOID(blyt_native_work) {
    volatile int x = 42;
    volatile int y = x + 1;
    (void)y;
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
        .c(C_SOURCE)
        .lua(LUA_SOURCE)
        .write(&project);

    let cart = build_debug_lua_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    // Look up blyt_native_work in the cart ELF symbol table.  With --debug
    // the symbol is in .symtab even though it is statically linked in.
    let addr = require_symbol_addr(&cart, "blyt_native_work");

    let orchestrator = repo_root().join("tests/gdb/run_sdl_hybrid_test.mjs");
    let mut cmd = Command::new("node");
    cmd.args([
        orchestrator.to_str().unwrap(),
        blytdebug().to_str().unwrap(),
        cart.to_str().unwrap(),
    ]);

    cmd.env("BLYT_GDB_BREAK_ADDR", format!("{addr:x}"));

    cmd.assert().success();
}

/// WASM hybrid: Lua cart that calls a native C function debuggable via both
/// DAP (Lua) and GDB (C).  Mirrors sdl_hybrid_gdb_and_dap on the WASM path.
///
/// Requires the WASM debug runtime (blytdebug.* built with BLYT_DAP+BLYT_GDB).
/// Silently skipped when blytdebug.js or the Lua SDK is not found.
///
/// For the equivalent test on the SDL2 path, see sdl_hybrid_gdb_and_dap.
#[test]
fn wasm_hybrid_gdb_and_dap() {
    require_sdk();
    require_lua_sdk();
    require_wasm_debug();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("wasm_hybrid_gdb_dap");

    const C_SOURCE: &str = r#"
#include "blyt.h"

BLYT_LUA_EXPORT_VOID(blyt_native_work) {
    volatile int x = 42;
    volatile int y = x + 1;
    (void)y;
}
"#;

    const LUA_SOURCE: &str = "\
function init()\n\
    local _ = 0\n\
    blyt_native_work()\n\
    local done = true\n\
    blyt_quit()\n\
end\n\
\n\
function update() end\n\
function draw()   end\n";

    CartProject::new()
        .c(C_SOURCE)
        .lua(LUA_SOURCE)
        .write(&project);

    let cart = build_debug_lua_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let addr = require_symbol_addr(&cart, "blyt_native_work");
    let wasm_dir = find_wasm_debug_dir();
    let orchestrator = repo_root().join("tests/gdb/run_wasm_hybrid_test.mjs");

    let mut cmd = Command::new("node");
    cmd.args([
        orchestrator.to_str().unwrap(),
        wasm_dir.to_str().unwrap(),
        cart.to_str().unwrap(),
    ]);

    cmd.env("BLYT_GDB_BREAK_ADDR", format!("{addr:x}"));

    cmd.timeout(std::time::Duration::from_secs(60))
        .assert()
        .success();
}

/* ── WASM equivalents of the extended SDL GDB protocol tests ─────────────── */

/// WASM GDB: read a known global variable address with the 'm' packet.
#[test]
fn wasm_c_cart_gdb_memory_read() {
    require_sdk();
    require_wasm_debug();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("wasm_gdb_mem");
    CartProject::new().c(DEBUG_C).write(&project);

    let cart = build_debug_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let bp_addr = require_symbol_addr(&cart, "blyt_debug_bp_target");
    let mem_addr = require_symbol_addr(&cart, "g_counter");
    let orchestrator = repo_root().join("tests/gdb/run_gdb_test.mjs");
    let mut cmd = Command::new("node");
    cmd.args([
        orchestrator.to_str().unwrap(),
        find_wasm_debug_dir().to_str().unwrap(),
        cart.to_str().unwrap(),
    ]);
    cmd.env("BLYT_GDB_BREAK_ADDR", format!("{bp_addr:x}"));
    cmd.env("BLYT_GDB_MEM_ADDR", format!("{mem_addr:x}"));
    cmd.assert().success();
}

/// WASM GDB: set three breakpoints and hit them in sequence.
#[test]
fn wasm_c_cart_gdb_multi_breakpoints() {
    require_sdk();
    require_wasm_debug();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("wasm_gdb_multi_bp");
    CartProject::new().c(DEBUG_C).write(&project);

    let cart = build_debug_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let addr1 = require_symbol_addr(&cart, "blyt_debug_bp_target");
    let addr2 = require_symbol_addr(&cart, "blyt_debug_bp2");
    let addr3 = require_symbol_addr(&cart, "blyt_debug_bp3");

    let orchestrator = repo_root().join("tests/gdb/run_wasm_multi_bp_test.mjs");
    Command::new("node")
        .args([
            orchestrator.to_str().unwrap(),
            find_wasm_debug_dir().to_str().unwrap(),
            cart.to_str().unwrap(),
        ])
        .env(
            "BLYT_GDB_BP_ADDRS",
            format!("{addr1:x},{addr2:x},{addr3:x}"),
        )
        .assert()
        .success();
}

/// WASM GDB: detach after a breakpoint stop — cart continues to completion.
#[test]
fn wasm_c_cart_gdb_detach() {
    require_sdk();
    require_wasm_debug();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("wasm_gdb_detach");
    CartProject::new().c(DEBUG_C).write(&project);

    let cart = build_debug_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let addr = require_symbol_addr(&cart, "blyt_debug_bp_target");
    let orchestrator = repo_root().join("tests/gdb/run_gdb_test.mjs");
    let mut cmd = Command::new("node");
    cmd.args([
        orchestrator.to_str().unwrap(),
        find_wasm_debug_dir().to_str().unwrap(),
        cart.to_str().unwrap(),
    ])
    .env("BLYT_GDB_DETACH", "1");
    cmd.env("BLYT_GDB_BREAK_ADDR", format!("{addr:x}"));
    cmd.assert().success();
}

/// WASM GDB: out-of-band interrupt (\x03) halts the cart and returns T02.
#[test]
fn wasm_c_cart_gdb_interrupt() {
    require_sdk();
    require_wasm_debug();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("wasm_gdb_interrupt");
    CartProject::new()
        .c("#include \"blyt.h\"\n\
             static int g_frame = 0;\n\
             void blyt_cart_init(void)   {}\n\
             void blyt_cart_update(void) { if (++g_frame >= 30000000) blyt_quit(); }\n\
             void blyt_cart_draw(void)   {}\n")
        .write(&project);

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let orchestrator = repo_root().join("tests/gdb/run_wasm_interrupt_test.mjs");
    Command::new("node")
        .args([
            orchestrator.to_str().unwrap(),
            find_wasm_debug_dir().to_str().unwrap(),
            cart.to_str().unwrap(),
        ])
        .assert()
        .success();
}

/// WASM GDB: qXfer:exec-file:read returns the cart path.
#[test]
fn wasm_c_cart_gdb_exec_file_query() {
    require_sdk();
    require_wasm_debug();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("wasm_gdb_exec_file");
    CartProject::new().c(DEBUG_C).write(&project);

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let orchestrator = repo_root().join("tests/gdb/run_gdb_test.mjs");
    Command::new("node")
        .args([
            orchestrator.to_str().unwrap(),
            find_wasm_debug_dir().to_str().unwrap(),
            cart.to_str().unwrap(),
        ])
        .env("BLYT_GDB_EXEC_FILE_CHECK", "1")
        .assert()
        .success();
}

/// WASM GDB: qXfer:libraries-svr4:read contains libblyt32.so.
#[test]
fn wasm_c_cart_gdb_library_list() {
    require_sdk();
    require_wasm_debug();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("wasm_gdb_lib_list");
    CartProject::new().c(DEBUG_C).write(&project);

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let orchestrator = repo_root().join("tests/gdb/run_gdb_test.mjs");
    Command::new("node")
        .args([
            orchestrator.to_str().unwrap(),
            find_wasm_debug_dir().to_str().unwrap(),
            cart.to_str().unwrap(),
        ])
        .env("BLYT_GDB_LIBRARY_CHECK", "1")
        .assert()
        .success();
}

/// WASM GDB: qXfer:features:read returns a target.xml with the RISC-V architecture.
#[test]
fn wasm_c_cart_gdb_features_query() {
    require_sdk();
    require_wasm_debug();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("wasm_gdb_features");
    CartProject::new().c(DEBUG_C).write(&project);
    let cart = build_cart(&project);
    assert!(cart.exists());

    let orchestrator = repo_root().join("tests/gdb/run_gdb_test.mjs");
    Command::new("node")
        .args([
            orchestrator.to_str().unwrap(),
            find_wasm_debug_dir().to_str().unwrap(),
            cart.to_str().unwrap(),
        ])
        .env("BLYT_GDB_FEATURES_CHECK", "1")
        .assert()
        .success();
}

/// WASM GDB: qProcessInfo returns the correct RISC-V 32-bit process triple.
#[test]
fn wasm_c_cart_gdb_process_info() {
    require_sdk();
    require_wasm_debug();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("wasm_gdb_procinfo");
    CartProject::new().c(DEBUG_C).write(&project);
    let cart = build_cart(&project);
    assert!(cart.exists());

    let orchestrator = repo_root().join("tests/gdb/run_gdb_test.mjs");
    Command::new("node")
        .args([
            orchestrator.to_str().unwrap(),
            find_wasm_debug_dir().to_str().unwrap(),
            cart.to_str().unwrap(),
        ])
        .env("BLYT_GDB_PROCESS_INFO", "1")
        .assert()
        .success();
}

/// WASM GDB: P (single register write) correctly updates a register.
#[test]
fn wasm_c_cart_gdb_register_write() {
    require_sdk();
    require_wasm_debug();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("wasm_gdb_reg_write");
    CartProject::new().c(DEBUG_C).write(&project);
    let cart = build_debug_cart(&project);
    assert!(cart.exists());

    let break_addr = format!("{:x}", require_symbol_addr(&cart, "blyt_debug_bp_target"));

    let orchestrator = repo_root().join("tests/gdb/run_gdb_test.mjs");
    Command::new("node")
        .args([
            orchestrator.to_str().unwrap(),
            find_wasm_debug_dir().to_str().unwrap(),
            cart.to_str().unwrap(),
        ])
        .env("BLYT_GDB_BREAK_ADDR", &break_addr)
        .env("BLYT_GDB_REGISTER_WRITE_CHECK", "1")
        .assert()
        .success();
}

/// WASM GDB: qThreadStopInfo returns T05 after a breakpoint stop.
#[test]
fn wasm_c_cart_gdb_thread_stop_info() {
    require_sdk();
    require_wasm_debug();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("wasm_gdb_tsi");
    CartProject::new().c(DEBUG_C).write(&project);
    let cart = build_debug_cart(&project);
    assert!(cart.exists());

    let break_addr = format!("{:x}", require_symbol_addr(&cart, "blyt_debug_bp_target"));

    let orchestrator = repo_root().join("tests/gdb/run_gdb_test.mjs");
    Command::new("node")
        .args([
            orchestrator.to_str().unwrap(),
            find_wasm_debug_dir().to_str().unwrap(),
            cart.to_str().unwrap(),
        ])
        .env("BLYT_GDB_BREAK_ADDR", &break_addr)
        .env("BLYT_GDB_THREAD_STOP_INFO", "1")
        .assert()
        .success();
}

/* ── Debug test cart source — shared by the extended protocol tests ───────── */

/// C source for a cart with multiple named functions and a writable global.
/// Used by: memory_read, multi_breakpoints, detach, interrupt, exec_file, library_list.
const DEBUG_C: &str = r#"
#include "blyt.h"
#include <stdint.h>

volatile uint32_t g_counter = 0;

__attribute__((noinline)) static void blyt_debug_bp2(void) { g_counter += 2; }
__attribute__((noinline)) static void blyt_debug_bp3(void) { g_counter += 3; }

void blyt_debug_bp_target(void) {
    g_counter = 1;
    blyt_debug_bp2();
    blyt_debug_bp3();
}

static int g_frame = 0;
void blyt_cart_init(void)   { blyt_debug_bp_target(); }
void blyt_cart_update(void) { if (++g_frame >= 3) blyt_quit(); }
void blyt_cart_draw(void)   {}
"#;

/// SDL2 GDB: read a known global variable address with the 'm' packet.
///
/// Sets a breakpoint at `blyt_debug_bp_target`, resumes, then reads the
/// 4 bytes at the address of `g_counter` and verifies an 8-char hex response.
///
/// Requires: blytplay with BLYT_GDB=ON, SDK, `readelf` on PATH.
#[test]
fn sdl_c_cart_gdb_memory_read() {
    require_sdk();
    require_gdb();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("sdl_gdb_mem");
    CartProject::new().c(DEBUG_C).write(&project);

    let cart = build_debug_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let bp_addr = require_symbol_addr(&cart, "blyt_debug_bp_target");
    let mem_addr = require_symbol_addr(&cart, "g_counter");

    let orchestrator = repo_root().join("tests/gdb/run_sdl_gdb_test.mjs");
    Command::new("node")
        .args([
            orchestrator.to_str().unwrap(),
            blytdebug().to_str().unwrap(),
            cart.to_str().unwrap(),
        ])
        .env("BLYT_GDB_BREAK_ADDR", format!("{bp_addr:x}"))
        .env("BLYT_GDB_MEM_ADDR", format!("{mem_addr:x}"))
        .assert()
        .success();
}

/// SDL2 GDB: set three breakpoints and hit them in sequence.
///
/// Sets Z0 at `blyt_debug_bp_target`, `blyt_debug_bp2`, and `blyt_debug_bp3`,
/// sends vCont;c three times, and asserts a T05 stop reply arrives each time.
///
/// Requires: blytplay with BLYT_GDB=ON, SDK, `readelf` on PATH.
#[test]
fn sdl_c_cart_gdb_multi_breakpoints() {
    require_sdk();
    require_gdb();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("sdl_gdb_multi_bp");
    CartProject::new().c(DEBUG_C).write(&project);

    let cart = build_debug_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let addr1 = require_symbol_addr(&cart, "blyt_debug_bp_target");
    let addr2 = require_symbol_addr(&cart, "blyt_debug_bp2");
    let addr3 = require_symbol_addr(&cart, "blyt_debug_bp3");

    let orchestrator = repo_root().join("tests/gdb/run_sdl_multi_bp_test.mjs");
    Command::new("node")
        .args([
            orchestrator.to_str().unwrap(),
            blytdebug().to_str().unwrap(),
            cart.to_str().unwrap(),
        ])
        .env(
            "BLYT_GDB_BP_ADDRS",
            format!("{addr1:x},{addr2:x},{addr3:x}"),
        )
        .assert()
        .success();
}

/// SDL2 GDB: detach after a breakpoint stop — cart continues to completion.
///
/// Sets a Z0 breakpoint, hits it, then sends 'D'.  The stub should return OK
/// and resume the cart without a debugger; blytplay should exit cleanly.
///
/// Requires: blytplay with BLYT_GDB=ON, SDK, `readelf` on PATH.
#[test]
fn sdl_c_cart_gdb_detach() {
    require_sdk();
    require_gdb();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("sdl_gdb_detach");
    CartProject::new().c(DEBUG_C).write(&project);

    let cart = build_debug_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let addr = require_symbol_addr(&cart, "blyt_debug_bp_target");
    let orchestrator = repo_root().join("tests/gdb/run_sdl_gdb_test.mjs");
    let mut cmd = Command::new("node");
    cmd.args([
        orchestrator.to_str().unwrap(),
        blytdebug().to_str().unwrap(),
        cart.to_str().unwrap(),
    ])
    .env("BLYT_GDB_DETACH", "1");
    cmd.env("BLYT_GDB_BREAK_ADDR", format!("{addr:x}"));
    cmd.assert().success();
}

/// SDL2 GDB: out-of-band interrupt (\x03) halts the cart and returns T02.
///
/// Sends vCont;c without a breakpoint, waits 100 ms, then sends the raw
/// \x03 byte.  Asserts that the stub responds with T02 (SIGINT).
///
/// Requires: blytplay with BLYT_GDB=ON, SDK.
#[test]
fn sdl_c_cart_gdb_interrupt() {
    require_sdk();
    require_gdb();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("sdl_gdb_interrupt");
    // Use a cart that runs for many frames — blytplay headless has no frame-rate
    // cap so 300 frames can finish in <<100ms.  30,000,000 gives several seconds
    // of wall time before the cart quits naturally (interrupted by \x03 first).
    CartProject::new()
        .c("#include \"blyt.h\"\n\
             static int g_frame = 0;\n\
             void blyt_cart_init(void)   {}\n\
             void blyt_cart_update(void) { if (++g_frame >= 30000000) blyt_quit(); }\n\
             void blyt_cart_draw(void)   {}\n")
        .write(&project);

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let orchestrator = repo_root().join("tests/gdb/run_sdl_interrupt_test.mjs");
    Command::new("node")
        .args([
            orchestrator.to_str().unwrap(),
            blytdebug().to_str().unwrap(),
            cart.to_str().unwrap(),
        ])
        .assert()
        .success();
}

/// SDL2 GDB: qXfer:exec-file:read returns the cart path.
///
/// After the GDB handshake, sends qXfer:exec-file:read::0,4000 and asserts
/// the response starts with 'l' and contains the cart filename.
///
/// Requires: blytplay with BLYT_GDB=ON, SDK.
#[test]
fn sdl_c_cart_gdb_exec_file_query() {
    require_sdk();
    require_gdb();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("sdl_gdb_exec_file");
    CartProject::new().c(DEBUG_C).write(&project);

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let orchestrator = repo_root().join("tests/gdb/run_sdl_gdb_test.mjs");
    Command::new("node")
        .args([
            orchestrator.to_str().unwrap(),
            blytdebug().to_str().unwrap(),
            cart.to_str().unwrap(),
        ])
        .env("BLYT_GDB_EXEC_FILE_CHECK", "1")
        .assert()
        .success();
}

/// SDL2 GDB: qXfer:libraries-svr4:read contains libblyt32.so.
///
/// After the GDB handshake, sends the libraries query and asserts the XML
/// response contains libblyt32.so — confirming the cart's dynamic linker
/// load addresses are correctly reported to the debugger.
///
/// Requires: blytplay with BLYT_GDB=ON, SDK.
#[test]
fn sdl_c_cart_gdb_library_list() {
    require_sdk();
    require_gdb();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("sdl_gdb_lib_list");
    CartProject::new().c(DEBUG_C).write(&project);

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let orchestrator = repo_root().join("tests/gdb/run_sdl_gdb_test.mjs");
    Command::new("node")
        .args([
            orchestrator.to_str().unwrap(),
            blytdebug().to_str().unwrap(),
            cart.to_str().unwrap(),
        ])
        .env("BLYT_GDB_LIBRARY_CHECK", "1")
        .assert()
        .success();
}

/// WASM GDB: relay announces "WASM runtime connected" once the cart connects.
///
/// The run_gdb_test.mjs orchestrator prints "[run_gdb_test] WASM runtime
/// connected" once the WASM cart's WebSocket GDB transport connects to the
/// relay.  This is the WASM-path equivalent of the "GDB: WASM ready" signal
/// that the VS Code extension waits for in the `blyt run` GDB relay.
///
/// Requires: WASM build with BLYT_GDB=ON.
#[test]
fn wasm_c_cart_gdb_runtime_connected_signal() {
    require_sdk();
    require_wasm_debug();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("wasm_gdb_signal");
    CartProject::new().c(DEBUG_C).write(&project);

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let orchestrator = repo_root().join("tests/gdb/run_gdb_test.mjs");
    let out = std::process::Command::new("node")
        .args([
            orchestrator.to_str().unwrap(),
            find_wasm_debug_dir().to_str().unwrap(),
            cart.to_str().unwrap(),
        ])
        .output()
        .expect("node must be on PATH");

    let combined = format!(
        "{}{}",
        String::from_utf8_lossy(&out.stdout),
        String::from_utf8_lossy(&out.stderr)
    );

    assert!(
        combined.contains("WASM runtime connected"),
        "expected 'WASM runtime connected' in output:\n{combined}"
    );
}

/// GDB: qXfer:features:read returns a target.xml with the RISC-V architecture.
///
/// Sends `qXfer:features:read:target.xml:0,4000` after handshake and verifies
/// the response starts with 'l' and contains an riscv architecture element.
///
/// Requires: blytplay with BLYT_GDB=ON, SDK.
#[test]
fn sdl_c_cart_gdb_features_query() {
    require_sdk();
    require_gdb();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("gdb_features");
    CartProject::new().c(DEBUG_C).write(&project);
    let cart = build_cart(&project);
    assert!(cart.exists());

    let orchestrator = repo_root().join("tests/gdb/run_sdl_gdb_test.mjs");
    Command::new("node")
        .args([
            orchestrator.to_str().unwrap(),
            blytdebug().to_str().unwrap(),
            cart.to_str().unwrap(),
        ])
        .env("BLYT_GDB_FEATURES_CHECK", "1")
        .assert()
        .success();
}

/// GDB: qProcessInfo returns the correct RISC-V 32-bit process triple.
///
/// Sends `qProcessInfo` after handshake and verifies the response contains
/// the `riscv32` triple and `endian:little`.
///
/// Requires: blytplay with BLYT_GDB=ON, SDK.
#[test]
fn sdl_c_cart_gdb_process_info() {
    require_sdk();
    require_gdb();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("gdb_procinfo");
    CartProject::new().c(DEBUG_C).write(&project);
    let cart = build_cart(&project);
    assert!(cart.exists());

    let orchestrator = repo_root().join("tests/gdb/run_sdl_gdb_test.mjs");
    Command::new("node")
        .args([
            orchestrator.to_str().unwrap(),
            blytdebug().to_str().unwrap(),
            cart.to_str().unwrap(),
        ])
        .env("BLYT_GDB_PROCESS_INFO", "1")
        .assert()
        .success();
}

/// GDB: P (single register write) correctly updates a register.
///
/// After stopping at a breakpoint, sends `P1:cdab3412` (write ra) then `p1`
/// (read ra) and verifies the value roundtrips correctly.
///
/// Requires: blytplay with BLYT_GDB=ON, SDK, readelf for symbol lookup.
#[test]
fn sdl_c_cart_gdb_register_write() {
    require_sdk();
    require_gdb();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("gdb_reg_write");
    CartProject::new().c(DEBUG_C).write(&project);
    let cart = build_debug_cart(&project);
    assert!(cart.exists());

    let break_addr = format!("{:x}", require_symbol_addr(&cart, "blyt_debug_bp_target"));

    let orchestrator = repo_root().join("tests/gdb/run_sdl_gdb_test.mjs");
    Command::new("node")
        .args([
            orchestrator.to_str().unwrap(),
            blytdebug().to_str().unwrap(),
            cart.to_str().unwrap(),
        ])
        .env("BLYT_GDB_BREAK_ADDR", &break_addr)
        .env("BLYT_GDB_REGISTER_WRITE_CHECK", "1")
        .assert()
        .success();
}

/// GDB: qThreadStopInfo returns T05 after a breakpoint stop.
///
/// After stopping at a breakpoint, sends `qThreadStopInfo1` and verifies the
/// response contains `T05` (the stop reason for a software breakpoint).
///
/// Requires: blytplay with BLYT_GDB=ON, SDK, readelf for symbol lookup.
#[test]
fn sdl_c_cart_gdb_thread_stop_info() {
    require_sdk();
    require_gdb();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("gdb_tsi");
    CartProject::new().c(DEBUG_C).write(&project);
    let cart = build_debug_cart(&project);
    assert!(cart.exists());

    let break_addr = format!("{:x}", require_symbol_addr(&cart, "blyt_debug_bp_target"));

    let orchestrator = repo_root().join("tests/gdb/run_sdl_gdb_test.mjs");
    Command::new("node")
        .args([
            orchestrator.to_str().unwrap(),
            blytdebug().to_str().unwrap(),
            cart.to_str().unwrap(),
        ])
        .env("BLYT_GDB_BREAK_ADDR", &break_addr)
        .env("BLYT_GDB_THREAD_STOP_INFO", "1")
        .assert()
        .success();
}
