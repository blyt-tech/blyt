mod common;

use assert_cmd::Command;
use common::{
    CartProject, blyt_bin, blytplay, build_lua_cart, require_cpp_sdk, require_lua_sdk,
    require_rust_riscv_target, require_sdk, require_wasm, run_cart_native, run_cart_wasm, sdk_dir,
};
use tempfile::TempDir;

/// `blyt build` with no blyt.build.yaml defaults to Lua; without Lua source
/// files it fails with a clear "no .lua files" error.
#[test]
fn build_lua_no_source_fails_with_error() {
    let tmp = tempfile::TempDir::new().unwrap();
    let project = tmp.path().join("no_manifest");
    // Create the project root but no src/game/lua/ directory.
    std::fs::create_dir_all(&project).unwrap();
    std::fs::write(project.join("blyt.info.yaml"), "name: no_manifest\n").unwrap();

    Command::new(blyt_bin())
        .args(["build", project.to_str().unwrap()])
        .env("BLYT_SDK_DIR", sdk_dir())
        .env("BLYT_OBJCOPY", sdk_dir().join("bin/blyt-objcopy"))
        .assert()
        .failure()
        .stderr(predicates::str::contains("no .lua files"));
}

/// A minimal Lua cart calls blyt32.debug.print in init() and produces the
/// expected output.
#[test]
fn lua_cart_debug_output() {
    require_sdk();
    require_lua_sdk();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("lua_hello");

    CartProject::new()
        .lua(
            r#"
function init()
    blyt32.debug.print("hello from lua")
end

function update()
    blyt.quit()
end

function draw() end
"#,
        )
        .write(&project);

    let cart = build_lua_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let output = Command::new(blytplay())
        .args(["--headless", cart.to_str().unwrap()])
        .assert()
        .success()
        .get_output()
        .stdout
        .clone();

    assert!(
        String::from_utf8_lossy(&output).contains("hello from lua"),
        "expected 'hello from lua' in output, got: {}",
        String::from_utf8_lossy(&output)
    );
}

#[test]
fn lua_cart_debug_output_wasm() {
    require_sdk();
    require_lua_sdk();
    require_wasm();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("lua_hello_wasm");

    CartProject::new()
        .lua(
            r#"
function init()
    blyt32.debug.print("hello from lua")
end

function update()
    blyt.quit()
end

function draw() end
"#,
        )
        .write(&project);

    let cart = build_lua_cart(&project);
    run_cart_wasm(&cart, "hello from lua");
}

// -------------------------------------------------------------------------
// Portable module exports — C, Rust, C++ — native and WASM
//
// All tests use BLYT_LUA_MODULE_EXPORT_* or #[lua_export(module = ...)]
// which route through the .lua_regtab and .lua_exports metadata sections.
// This is cross-runtime compatible: rv32emu-native and WASM both supported.
// cart_lua_modules and raw lua_State * access are forbidden from cart code.
// -------------------------------------------------------------------------

fn build_lua_c_lib_module_cart(tmp: &std::path::Path) -> std::path::PathBuf {
    let project = tmp.join("lua_c_lib_module");
    CartProject::new()
        .lib_file(
            "mathlib",
            "mathlib.c",
            "#include \"blyt.h\"\n\
             BLYT_LUA_MODULE_EXPORT_I32(mathlib, double, int32_t x) { return x * 2; }\n",
        )
        .lua(
            r#"
function init()
    local m = require("mathlib")
    local result = m.double(5)
    if result == 10 then
        blyt32.debug.print("lua+c ok")
    else
        blyt32.debug.print("lua+c wrong")
    end
end

function update() blyt.quit() end
function draw() end
"#,
        )
        .write(&project);
    build_lua_cart(&project)
}

#[test]
fn lua_cart_calls_c_lib() {
    require_sdk();
    require_lua_sdk();
    let tmp = TempDir::new().unwrap();
    let cart = build_lua_c_lib_module_cart(tmp.path());
    run_cart_native(&cart, "lua+c ok");
}

#[test]
fn lua_cart_calls_c_lib_wasm() {
    require_sdk();
    require_lua_sdk();
    require_wasm();
    let tmp = TempDir::new().unwrap();
    let cart = build_lua_c_lib_module_cart(tmp.path());
    run_cart_wasm(&cart, "lua+c ok");
}

fn build_lua_rust_lib_module_cart(tmp: &std::path::Path) -> std::path::PathBuf {
    let project = tmp.join("lua_rust_lib_module");
    CartProject::new()
        .rust(
            r#"#![no_std]
extern crate blyt;
use blyt::lua_export;

#[lua_export(module = "rustlib")]
fn square(x: i32) -> i32 { x * x }
"#,
        )
        .lua(
            r#"
function init()
    local m = require("rustlib")
    local result = m.square(7)
    if result == 49 then
        blyt32.debug.print("lua+rust ok")
    else
        blyt32.debug.print("lua+rust wrong")
    end
end

function update() blyt.quit() end
function draw() end
"#,
        )
        .write(&project);
    build_lua_cart(&project)
}

#[test]
fn lua_cart_calls_rust_lib() {
    require_sdk();
    require_lua_sdk();
    require_rust_riscv_target();
    let tmp = TempDir::new().unwrap();
    let cart = build_lua_rust_lib_module_cart(tmp.path());
    run_cart_native(&cart, "lua+rust ok");
}

#[test]
fn lua_cart_calls_rust_lib_wasm() {
    require_sdk();
    require_lua_sdk();
    require_rust_riscv_target();
    require_wasm();
    let tmp = TempDir::new().unwrap();
    let cart = build_lua_rust_lib_module_cart(tmp.path());
    run_cart_wasm(&cart, "lua+rust ok");
}

fn build_lua_cpp_lib_module_cart(tmp: &std::path::Path) -> std::path::PathBuf {
    let project = tmp.join("lua_cpp_lib_module");
    CartProject::new()
        .lib_file(
            "cpplib",
            "cpplib.cpp",
            r#"#include "blyt.h"

extern "C" {
BLYT_LUA_MODULE_EXPORT_I32(cpplib, square, int32_t x) { return x * x; }
}
"#,
        )
        .lua(
            r#"
function init()
    local m = require("cpplib")
    local result = m.square(9)
    if result == 81 then
        blyt32.debug.print("lua+cpp ok")
    else
        blyt32.debug.print("lua+cpp wrong")
    end
end

function update() blyt.quit() end
function draw() end
"#,
        )
        .write(&project);
    build_lua_cart(&project)
}

#[test]
fn lua_cart_calls_cpp_lib() {
    require_sdk();
    require_cpp_sdk();
    require_lua_sdk();
    let tmp = TempDir::new().unwrap();
    let cart = build_lua_cpp_lib_module_cart(tmp.path());
    run_cart_native(&cart, "lua+cpp ok");
}

#[test]
fn lua_cart_calls_cpp_lib_wasm() {
    require_sdk();
    require_cpp_sdk();
    require_lua_sdk();
    require_wasm();
    let tmp = TempDir::new().unwrap();
    let cart = build_lua_cpp_lib_module_cart(tmp.path());
    run_cart_wasm(&cart, "lua+cpp ok");
}

/// The portable cross-runtime pattern: C code receives a value as an argument
/// rather than reading Lua globals via lua_getglobal (which is WASM-incompatible).
fn build_c_increment_cart(tmp: &std::path::Path) -> std::path::PathBuf {
    let project = tmp.join("c_increment");
    CartProject::new()
        .lib_file(
            "inclib",
            "inclib.c",
            "#include \"blyt.h\"\n\
             BLYT_LUA_EXPORT_I32(increment, int32_t x) { return x + 1; }\n",
        )
        .lua(
            r#"
function init()
    local result = increment(41)
    if result == 42 then
        blyt32.debug.print("c-drives-lua ok")
    else
        blyt32.debug.print("c-drives-lua wrong")
    end
end

function update() blyt.quit() end
function draw() end
"#,
        )
        .write(&project);
    build_lua_cart(&project)
}

#[test]
fn c_lib_drives_lua_vm() {
    require_sdk();
    require_lua_sdk();
    let tmp = TempDir::new().unwrap();
    let cart = build_c_increment_cart(tmp.path());
    run_cart_native(&cart, "c-drives-lua ok");
}

#[test]
fn c_lib_drives_lua_vm_wasm() {
    require_sdk();
    require_lua_sdk();
    require_wasm();
    let tmp = TempDir::new().unwrap();
    let cart = build_c_increment_cart(tmp.path());
    run_cart_wasm(&cart, "c-drives-lua ok");
}

// -------------------------------------------------------------------------
// Lua + src/game/ native code tests
// -------------------------------------------------------------------------

fn build_lua_c_game_module_cart(tmp: &std::path::Path) -> std::path::PathBuf {
    let project = tmp.join("lua_c_game_module");
    CartProject::new()
        .c("#include \"blyt.h\"\n\
             BLYT_LUA_MODULE_EXPORT_I32(gamelib, negate, int32_t x) { return -x; }\n")
        .lua(
            r#"
function init()
    local m = require("gamelib")
    local result = m.negate(42)
    if result == -42 then
        blyt32.debug.print("lua+c-game ok")
    else
        blyt32.debug.print("lua+c-game wrong")
    end
end

function update() blyt.quit() end
function draw() end
"#,
        )
        .write(&project);
    build_lua_cart(&project)
}

#[test]
fn lua_cart_with_c_game_code() {
    require_sdk();
    require_lua_sdk();
    let tmp = TempDir::new().unwrap();
    let cart = build_lua_c_game_module_cart(tmp.path());
    run_cart_native(&cart, "lua+c-game ok");
}

#[test]
fn lua_cart_with_c_game_code_wasm() {
    require_sdk();
    require_lua_sdk();
    require_wasm();
    let tmp = TempDir::new().unwrap();
    let cart = build_lua_c_game_module_cart(tmp.path());
    run_cart_wasm(&cart, "lua+c-game ok");
}

fn build_lua_cpp_game_module_cart(tmp: &std::path::Path) -> std::path::PathBuf {
    let project = tmp.join("lua_cpp_game_module");
    CartProject::new()
        .cpp(
            r#"#include "blyt.h"

extern "C" {
BLYT_LUA_MODULE_EXPORT_I32(cppgamelib, square, int32_t x) { return x * x; }
}
"#,
        )
        .lua(
            r#"
function init()
    local m = require("cppgamelib")
    local result = m.square(7)
    if result == 49 then
        blyt32.debug.print("lua+cpp-game ok")
    else
        blyt32.debug.print("lua+cpp-game wrong")
    end
end

function update() blyt.quit() end
function draw() end
"#,
        )
        .write(&project);
    build_lua_cart(&project)
}

#[test]
fn lua_cart_with_cpp_game_code() {
    require_sdk();
    require_cpp_sdk();
    require_lua_sdk();
    let tmp = TempDir::new().unwrap();
    let cart = build_lua_cpp_game_module_cart(tmp.path());
    run_cart_native(&cart, "lua+cpp-game ok");
}

#[test]
fn lua_cart_with_cpp_game_code_wasm() {
    require_sdk();
    require_cpp_sdk();
    require_lua_sdk();
    require_wasm();
    let tmp = TempDir::new().unwrap();
    let cart = build_lua_cpp_game_module_cart(tmp.path());
    run_cart_wasm(&cart, "lua+cpp-game ok");
}

fn build_lua_rust_hybrid_cart(tmp: &std::path::Path) -> std::path::PathBuf {
    let project = tmp.join("lua_rust_hybrid");
    CartProject::new()
        .rust(
            r#"#![no_std]
extern crate blyt;
use blyt::lua_export;

#[lua_export]
fn double(x: i32) -> i32 { x * 2 }
"#,
        )
        .lua(
            r#"
function init()
    local r = double(21)
    if r ~= 42 then error("expected 42, got " .. tostring(r)) end
    blyt32.debug.print("lua+rust ok")
end
function update() blyt.quit() end
function draw() end
"#,
        )
        .write(&project);
    build_lua_cart(&project)
}

#[test]
fn lua_rust_hybrid_native() {
    require_sdk();
    require_lua_sdk();
    require_rust_riscv_target();
    let tmp = TempDir::new().unwrap();
    let cart = build_lua_rust_hybrid_cart(tmp.path());
    run_cart_native(&cart, "lua+rust ok");
}

#[test]
fn lua_rust_hybrid_wasm() {
    require_sdk();
    require_lua_sdk();
    require_rust_riscv_target();
    require_wasm();
    let tmp = TempDir::new().unwrap();
    let cart = build_lua_rust_hybrid_cart(tmp.path());
    run_cart_wasm(&cart, "lua+rust ok");
}

fn build_lua_c_hybrid_cart(tmp: &std::path::Path) -> std::path::PathBuf {
    let project = tmp.join("lua_c_hybrid");
    CartProject::new()
        .c("#include \"blyt.h\"\n\
             BLYT_LUA_EXPORT_I32(add_one, int32_t x) { return x + 1; }\n")
        .lua(
            r#"
function init()
    local r = add_one(41)
    if r ~= 42 then error("expected 42, got " .. tostring(r)) end
    blyt32.debug.print("lua+c ok")
end
function update() blyt.quit() end
function draw() end
"#,
        )
        .write(&project);
    build_lua_cart(&project)
}

#[test]
fn lua_c_hybrid_native() {
    require_sdk();
    require_lua_sdk();
    let tmp = TempDir::new().unwrap();
    let cart = build_lua_c_hybrid_cart(tmp.path());
    run_cart_native(&cart, "lua+c ok");
}

#[test]
fn lua_c_hybrid_wasm() {
    require_sdk();
    require_lua_sdk();
    require_wasm();
    let tmp = TempDir::new().unwrap();
    let cart = build_lua_c_hybrid_cart(tmp.path());
    run_cart_wasm(&cart, "lua+c ok");
}

fn build_lua_c_rust_exports_cart(tmp: &std::path::Path) -> std::path::PathBuf {
    let project = tmp.join("lua_c_rust_exports");
    CartProject::new()
        .c("#include \"blyt.h\"\n\
             BLYT_LUA_EXPORT_I32(c_triple, int32_t x) { return x * 3; }\n")
        .rust(
            r#"#![no_std]
extern crate blyt;
use blyt::lua_export;

#[lua_export]
fn rust_double(x: i32) -> i32 { x * 2 }
"#,
        )
        .lua(
            r#"
function init()
    local r1 = c_triple(7)
    local r2 = rust_double(21)
    if r1 ~= 21 then error("c_triple wrong: " .. tostring(r1)) end
    if r2 ~= 42 then error("rust_double wrong: " .. tostring(r2)) end
    blyt32.debug.print("lua+c+rust exports ok")
end
function update() blyt.quit() end
function draw() end
"#,
        )
        .write(&project);
    build_lua_cart(&project)
}

#[test]
fn lua_c_rust_exports_native() {
    require_sdk();
    require_lua_sdk();
    require_rust_riscv_target();
    let tmp = TempDir::new().unwrap();
    let cart = build_lua_c_rust_exports_cart(tmp.path());
    run_cart_native(&cart, "lua+c+rust exports ok");
}

#[test]
fn lua_c_rust_exports_wasm() {
    require_sdk();
    require_lua_sdk();
    require_rust_riscv_target();
    require_wasm();
    let tmp = TempDir::new().unwrap();
    let cart = build_lua_c_rust_exports_cart(tmp.path());
    run_cart_wasm(&cart, "lua+c+rust exports ok");
}

fn build_lua_rust_c_chain_cart(tmp: &std::path::Path) -> std::path::PathBuf {
    let project = tmp.join("lua_rust_c_chain");
    CartProject::new()
        .c("int c_double(int x) { return x * 2; }\n")
        .rust(
            r#"#![no_std]
extern crate blyt;
use blyt::lua_export;

extern "C" { fn c_double(x: i32) -> i32; }

#[lua_export(name = "compute")]
fn bridge_compute(x: i32) -> i32 { unsafe { c_double(x) } }
"#,
        )
        .lua(
            r#"
function init()
    local r = compute(21)
    if r ~= 42 then error("expected 42, got " .. tostring(r)) end
    blyt32.debug.print("lua->rust->c ok")
end
function update() blyt.quit() end
function draw() end
"#,
        )
        .write(&project);
    build_lua_cart(&project)
}

#[test]
fn lua_rust_c_chain_native() {
    require_sdk();
    require_lua_sdk();
    require_rust_riscv_target();
    let tmp = TempDir::new().unwrap();
    let cart = build_lua_rust_c_chain_cart(tmp.path());
    run_cart_native(&cart, "lua->rust->c ok");
}

#[test]
fn lua_rust_c_chain_wasm() {
    require_sdk();
    require_lua_sdk();
    require_rust_riscv_target();
    require_wasm();
    let tmp = TempDir::new().unwrap();
    let cart = build_lua_rust_c_chain_cart(tmp.path());
    run_cart_wasm(&cart, "lua->rust->c ok");
}

fn build_lua_cpp_hybrid_cart(tmp: &std::path::Path) -> std::path::PathBuf {
    let project = tmp.join("lua_cpp_hybrid");
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
    local r = cube(3)
    if r ~= 27 then error("expected 27, got " .. tostring(r)) end
    blyt32.debug.print("lua+cpp ok")
end
function update() blyt.quit() end
function draw() end
"#,
        )
        .write(&project);
    build_lua_cart(&project)
}

#[test]
fn lua_cpp_hybrid_native() {
    require_sdk();
    require_cpp_sdk();
    require_lua_sdk();
    let tmp = TempDir::new().unwrap();
    let cart = build_lua_cpp_hybrid_cart(tmp.path());
    run_cart_native(&cart, "lua+cpp ok");
}

#[test]
fn lua_cpp_hybrid_wasm() {
    require_sdk();
    require_cpp_sdk();
    require_lua_sdk();
    require_wasm();
    let tmp = TempDir::new().unwrap();
    let cart = build_lua_cpp_hybrid_cart(tmp.path());
    run_cart_wasm(&cart, "lua+cpp ok");
}

fn build_lua_c_lib_export_cart(tmp: &std::path::Path) -> std::path::PathBuf {
    let project = tmp.join("lua_c_lib_export");
    CartProject::new()
        .lib_file(
            "triplelib",
            "triplelib.c",
            "#include \"blyt.h\"\n\
             BLYT_LUA_EXPORT_I32(lib_triple, int32_t x) { return x * 3; }\n",
        )
        .lua(
            r#"
function init()
    local r = lib_triple(14)
    if r ~= 42 then error("expected 42, got " .. tostring(r)) end
    blyt32.debug.print("lua+c-lib ok")
end
function update() blyt.quit() end
function draw() end
"#,
        )
        .write(&project);
    build_lua_cart(&project)
}

#[test]
fn lua_c_lib_export_native() {
    require_sdk();
    require_lua_sdk();
    let tmp = TempDir::new().unwrap();
    let cart = build_lua_c_lib_export_cart(tmp.path());
    run_cart_native(&cart, "lua+c-lib ok");
}

#[test]
fn lua_c_lib_export_wasm() {
    require_sdk();
    require_lua_sdk();
    require_wasm();
    let tmp = TempDir::new().unwrap();
    let cart = build_lua_c_lib_export_cart(tmp.path());
    run_cart_wasm(&cart, "lua+c-lib ok");
}

// -------------------------------------------------------------------------
// New module export tests — dedicated coverage for BLYT_LUA_MODULE_EXPORT_*
// and #[lua_export(module = ...)] on both native and WASM runtimes.
// -------------------------------------------------------------------------

fn build_lua_module_export_cart(tmp: &std::path::Path) -> std::path::PathBuf {
    let project = tmp.join("lua_module_export");
    CartProject::new()
        .c("#include \"blyt.h\"\n\
             BLYT_LUA_MODULE_EXPORT_I32(ops, triple, int32_t x) { return x * 3; }\n")
        .lua(
            r#"
function init()
    local m = require("ops")
    local result = m.triple(14)
    if result == 42 then
        blyt32.debug.print("module export ok")
    else
        blyt32.debug.print("module export wrong")
    end
end

function update() blyt.quit() end
function draw() end
"#,
        )
        .write(&project);
    build_lua_cart(&project)
}

#[test]
fn lua_module_export_native() {
    require_sdk();
    require_lua_sdk();
    let tmp = TempDir::new().unwrap();
    let cart = build_lua_module_export_cart(tmp.path());
    run_cart_native(&cart, "module export ok");
}

#[test]
fn lua_module_export_wasm() {
    require_sdk();
    require_lua_sdk();
    require_wasm();
    let tmp = TempDir::new().unwrap();
    let cart = build_lua_module_export_cart(tmp.path());
    run_cart_wasm(&cart, "module export ok");
}

fn build_lua_rust_module_export_cart(tmp: &std::path::Path) -> std::path::PathBuf {
    let project = tmp.join("lua_rust_module_export");
    CartProject::new()
        .rust(
            r#"#![no_std]
extern crate blyt;
use blyt::lua_export;

#[lua_export(module = "ops")]
fn negate(x: i32) -> i32 { -x }
"#,
        )
        .lua(
            r#"
function init()
    local m = require("ops")
    local result = m.negate(42)
    if result == -42 then
        blyt32.debug.print("rust module export ok")
    else
        blyt32.debug.print("rust module export wrong")
    end
end

function update() blyt.quit() end
function draw() end
"#,
        )
        .write(&project);
    build_lua_cart(&project)
}

#[test]
fn lua_rust_module_export_native() {
    require_sdk();
    require_lua_sdk();
    require_rust_riscv_target();
    let tmp = TempDir::new().unwrap();
    let cart = build_lua_rust_module_export_cart(tmp.path());
    run_cart_native(&cart, "rust module export ok");
}

#[test]
fn lua_rust_module_export_wasm() {
    require_sdk();
    require_lua_sdk();
    require_rust_riscv_target();
    require_wasm();
    let tmp = TempDir::new().unwrap();
    let cart = build_lua_rust_module_export_cart(tmp.path());
    run_cart_wasm(&cart, "rust module export ok");
}
