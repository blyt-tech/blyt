mod common;

use assert_cmd::Command;
use common::{
    CartProject, blyt_bin, blytplay, build_lua_cart, require_cpp_sdk, require_lua_sdk,
    require_rust_riscv_target, require_sdk, sdk_dir,
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

/// A Lua cart calls a C library function via the cart_lua_modules mechanism.
///
/// The C library defines `cart_lua_modules` which registers a "mathlib" Lua
/// module.  The Lua cart requires("mathlib") and calls mathlib.add().
#[test]
fn lua_cart_calls_c_lib() {
    require_sdk();
    require_lua_sdk();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("lua_c_lib");

    CartProject::new()
        // C library with cart_lua_modules registration
        .lib_file(
            "mathlib",
            "include/mathlib.h",
            "#ifndef MATHLIB_H\n#define MATHLIB_H\n\
             #ifdef __cplusplus\nextern \"C\" {\n#endif\n\
             void mathlib_lua_register(void *L);\n\
             #ifdef __cplusplus\n}\n#endif\n#endif\n",
        )
        .lib_file(
            "mathlib",
            "mathlib.c",
            r#"
#include "lua.h"
#include "lauxlib.h"

static int l_add(lua_State *L) {
    int a = (int)luaL_checkinteger(L, 1);
    int b = (int)luaL_checkinteger(L, 2);
    lua_pushinteger(L, a + b);
    return 1;
}

static const luaL_Reg mathlib_funcs[] = {
    {"add", l_add},
    {NULL, NULL}
};

int luaopen_mathlib(lua_State *L) {
    luaL_newlib(L, mathlib_funcs);
    return 1;
}

void cart_lua_modules(lua_State *L) {
    luaL_requiref(L, "mathlib", luaopen_mathlib, 1);
    lua_pop(L, 1);
}
"#,
        )
        .lua(
            r#"
function init()
    local m = require("mathlib")
    local result = m.add(3, 4)
    if result == 7 then
        blyt32.debug.print("lua+c ok")
    else
        blyt32.debug.print("lua+c wrong")
    end
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
        String::from_utf8_lossy(&output).contains("lua+c ok"),
        "expected 'lua+c ok' in output, got: {}",
        String::from_utf8_lossy(&output)
    );
}

/// A Lua cart calls a Rust library function via the cart_lua_modules mechanism.
///
/// The Rust library defines `cart_lua_modules` using raw Lua C FFI, registering
/// a "rustlib" module.  The Lua cart requires("rustlib") and calls
/// rustlib.multiply().
#[test]
fn lua_cart_calls_rust_lib() {
    require_sdk();
    require_lua_sdk();
    require_rust_riscv_target();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("lua_rust_lib");

    // The Rust lib defines cart_lua_modules using raw Lua C FFI.  It registers
    // a "rustlib" Lua module with a single multiply() function implemented in Rust.
    // No blyt SDK dependency: the lib only calls lua_* symbols exported by
    // libblyt32lua.so and resolved by the cart dynlinker at runtime.
    let rust_lib_src = r#"#![no_std]

// staticlib requires a panic handler even with panic=abort (the symbol must
// exist; it is never called because -C panic=abort generates abort instead).
#[panic_handler]
fn panic(_: &core::panic::PanicInfo) -> ! {
    loop {}
}

use core::ffi::{c_int, c_void};

type LuaState = c_void;

extern "C" {
    fn lua_createtable(L: *mut LuaState, narr: c_int, nrec: c_int);
    fn lua_pushcclosure(
        L: *mut LuaState,
        f: unsafe extern "C" fn(*mut LuaState) -> c_int,
        n: c_int,
    );
    fn lua_setfield(L: *mut LuaState, idx: c_int, k: *const u8);
    fn lua_pushinteger(L: *mut LuaState, n: c_int);
    fn luaL_checkinteger(L: *mut LuaState, arg: c_int) -> c_int;
    fn luaL_requiref(
        L: *mut LuaState,
        modname: *const u8,
        openf: unsafe extern "C" fn(*mut LuaState) -> c_int,
        glb: c_int,
    );
    fn lua_settop(L: *mut LuaState, idx: c_int);
}

unsafe extern "C" fn l_multiply(L: *mut LuaState) -> c_int {
    let a = luaL_checkinteger(L, 1);
    let b = luaL_checkinteger(L, 2);
    lua_pushinteger(L, a * b);
    1
}

unsafe extern "C" fn luaopen_rustlib(L: *mut LuaState) -> c_int {
    lua_createtable(L, 0, 1);
    lua_pushcclosure(L, l_multiply, 0);
    lua_setfield(L, -2, b"multiply\0".as_ptr());
    1
}

#[no_mangle]
pub unsafe extern "C" fn cart_lua_modules(L: *mut LuaState) {
    luaL_requiref(L, b"rustlib\0".as_ptr(), luaopen_rustlib, 1);
    lua_settop(L, -2); // lua_pop(L, 1)
}
"#;

    // Use lib_file to write the Cargo.toml with crate-type = ["staticlib"] so
    // blyt build can compile the Rust lib to a .a and link it into the cart.
    // (rust_lib() generates crate-type-less Cargo.toml suitable only for
    // --config injection into a parent Rust game crate.)
    CartProject::new()
        .lib_file(
            "rustlib",
            "Cargo.toml",
            "[package]\nname = \"rustlib\"\nversion = \"0.1.0\"\nedition = \"2021\"\npublish = false\n\n[lib]\ncrate-type = [\"staticlib\"]\n",
        )
        .lib_file("rustlib", "src/lib.rs", rust_lib_src)
        .lua(
            r#"
function init()
    local m = require("rustlib")
    local result = m.multiply(6, 7)
    if result == 42 then
        blyt32.debug.print("lua+rust ok")
    else
        blyt32.debug.print("lua+rust wrong")
    end
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
        String::from_utf8_lossy(&output).contains("lua+rust ok"),
        "expected 'lua+rust ok' in output, got: {}",
        String::from_utf8_lossy(&output)
    );
}

/// A Lua cart calls a C++ library function via the cart_lua_modules mechanism.
///
/// The C++ library defines `extern "C" void cart_lua_modules(lua_State *L)`
/// which registers a "cpplib" Lua module backed by a C++ function.  The Lua
/// cart requires("cpplib") and calls cpplib.square().
#[test]
fn lua_cart_calls_cpp_lib() {
    require_sdk();
    require_cpp_sdk();
    require_lua_sdk();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("lua_cpp_lib");

    CartProject::new()
        // C++ library: defines cart_lua_modules with extern "C" so the cart
        // dynlinker finds it as a plain C symbol; uses a C++ function internally.
        .lib_file(
            "cpplib",
            "cpplib.cpp",
            // Lua headers have no extern "C" guards, so wrap them explicitly to
            // prevent C++ name mangling of lua_* / luaL_* symbols.
            r#"
extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

// C++ implementation (unguarded — purely internal, no Lua API exposure).
static int cpp_square(int x) { return x * x; }

extern "C" {

static int l_square(lua_State *L) {
    int x = (int)luaL_checkinteger(L, 1);
    lua_pushinteger(L, cpp_square(x));
    return 1;
}

static const luaL_Reg cpplib_funcs[] = {
    {"square", l_square},
    {NULL, NULL}
};

int luaopen_cpplib(lua_State *L) {
    luaL_newlib(L, cpplib_funcs);
    return 1;
}

void cart_lua_modules(lua_State *L) {
    luaL_requiref(L, "cpplib", luaopen_cpplib, 1);
    lua_pop(L, 1);
}

} // extern "C"
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
        String::from_utf8_lossy(&output).contains("lua+cpp ok"),
        "expected 'lua+cpp ok' in output, got: {}",
        String::from_utf8_lossy(&output)
    );
}

/// Cart C library code drives the Lua VM: calls back into Lua from within a
/// C function registered as a Lua module entry point.
///
/// This distinguishes cart-native code using the Lua API from the runtime
/// (libblyt32lua.so) doing so.  The C lib registers a Lua module; that module's
/// function retrieves a Lua global (set by the Lua cart) and returns it
/// incremented, exercising lua_getglobal / lua_tointegerx / lua_pushinteger
/// from within cart C code.
#[test]
fn c_lib_drives_lua_vm() {
    require_sdk();
    require_lua_sdk();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("c_drives_lua");

    CartProject::new()
        .lib_file(
            "cdrivelib",
            "cdrivelib.c",
            r#"
#include "lua.h"
#include "lauxlib.h"

/* l_get_base: reads the Lua global "base_value" set by the cart and returns
 * it incremented by 1.  Exercises lua_getglobal and lua_tointegerx from
 * cart C code rather than from the runtime. */
static int l_get_base(lua_State *L) {
    lua_getglobal(L, "base_value");
    int ok = 0;
    int val = (int)lua_tointegerx(L, -1, &ok);
    lua_pop(L, 1);
    if (!ok) {
        return luaL_error(L, "base_value is not an integer");
    }
    lua_pushinteger(L, val + 1);
    return 1;
}

static const luaL_Reg cdrivelib_funcs[] = {
    {"get_base", l_get_base},
    {NULL, NULL}
};

int luaopen_cdrivelib(lua_State *L) {
    luaL_newlib(L, cdrivelib_funcs);
    return 1;
}

void cart_lua_modules(lua_State *L) {
    luaL_requiref(L, "cdrivelib", luaopen_cdrivelib, 1);
    lua_pop(L, 1);
}
"#,
        )
        .lua(
            r#"
base_value = 41

function init()
    local m = require("cdrivelib")
    local result = m.get_base()
    if result == 42 then
        blyt32.debug.print("c-drives-lua ok")
    else
        blyt32.debug.print("c-drives-lua wrong")
    end
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
        String::from_utf8_lossy(&output).contains("c-drives-lua ok"),
        "expected 'c-drives-lua ok' in output, got: {}",
        String::from_utf8_lossy(&output)
    );
}

// -------------------------------------------------------------------------
// Lua + src/game/ native code tests
//
// These verify that a Lua cart can ship C, C++, or Rust game code under
// src/game/ (not src/lib/).  The native code defines cart_lua_modules and
// is compiled/linked alongside the Lua bytecode.
// -------------------------------------------------------------------------

/// A Lua cart with C game code under src/game/c/ that registers a Lua module.
///
/// This is distinct from lua_cart_calls_c_lib (which uses src/lib/): game-level
/// C code is compiled as plain object files and linked directly, so no archive
/// step is involved.
#[test]
fn lua_cart_with_c_game_code() {
    require_sdk();
    require_lua_sdk();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("lua_c_game");

    CartProject::new()
        .c(r#"
#include "lua.h"
#include "lauxlib.h"

static int l_negate(lua_State *L) {
    int x = (int)luaL_checkinteger(L, 1);
    lua_pushinteger(L, -x);
    return 1;
}

static const luaL_Reg gamelib_funcs[] = {
    {"negate", l_negate},
    {NULL, NULL}
};

int luaopen_gamelib(lua_State *L) {
    luaL_newlib(L, gamelib_funcs);
    return 1;
}

void cart_lua_modules(lua_State *L) {
    luaL_requiref(L, "gamelib", luaopen_gamelib, 1);
    lua_pop(L, 1);
}
"#)
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
        String::from_utf8_lossy(&output).contains("lua+c-game ok"),
        "expected 'lua+c-game ok' in output, got: {}",
        String::from_utf8_lossy(&output)
    );
}

/// A Lua cart with C++ game code under src/game/c++/ that registers a Lua module.
#[test]
fn lua_cart_with_cpp_game_code() {
    require_sdk();
    require_cpp_sdk();
    require_lua_sdk();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("lua_cpp_game");

    CartProject::new()
        // Lua headers have no extern "C" guards — wrap includes explicitly.
        .cpp(
            r#"
extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

static int cpp_square(int x) { return x * x; }

extern "C" {

static int l_square(lua_State *L) {
    int x = (int)luaL_checkinteger(L, 1);
    lua_pushinteger(L, cpp_square(x));
    return 1;
}

static const luaL_Reg gamelib_funcs[] = {
    {"square", l_square},
    {NULL, NULL}
};

int luaopen_cppgamelib(lua_State *L) {
    luaL_newlib(L, gamelib_funcs);
    return 1;
}

void cart_lua_modules(lua_State *L) {
    luaL_requiref(L, "cppgamelib", luaopen_cppgamelib, 1);
    lua_pop(L, 1);
}

} // extern "C"
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
        String::from_utf8_lossy(&output).contains("lua+cpp-game ok"),
        "expected 'lua+cpp-game ok' in output, got: {}",
        String::from_utf8_lossy(&output)
    );
}

/// A Lua cart with Rust game code under src/game/rust/ that registers a Lua module.
///
/// The Rust game crate defines cart_lua_modules using raw Lua C FFI.  The blyt
/// SDK crate (auto-injected as a dependency) provides the #[panic_handler] and
/// #[global_allocator] so the Rust game code integrates cleanly with the cart.
#[test]
fn lua_cart_with_rust_game_code() {
    require_sdk();
    require_lua_sdk();
    require_rust_riscv_target();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("lua_rust_game");

    // The blyt SDK crate (injected via --config at build time) provides
    // #[panic_handler] and #[global_allocator].  `extern crate blyt` forces the
    // crate into the compilation graph even though no blyt items are directly
    // called — without this the panic handler from blyt is not discovered.
    let rust_game_src = r#"#![no_std]
extern crate blyt;

use core::ffi::{c_int, c_void};

type LuaState = c_void;

extern "C" {
    fn lua_createtable(L: *mut LuaState, narr: c_int, nrec: c_int);
    fn lua_pushcclosure(
        L: *mut LuaState,
        f: unsafe extern "C" fn(*mut LuaState) -> c_int,
        n: c_int,
    );
    fn lua_setfield(L: *mut LuaState, idx: c_int, k: *const u8);
    fn lua_pushinteger(L: *mut LuaState, n: c_int);
    fn luaL_checkinteger(L: *mut LuaState, arg: c_int) -> c_int;
    fn luaL_requiref(
        L: *mut LuaState,
        modname: *const u8,
        openf: unsafe extern "C" fn(*mut LuaState) -> c_int,
        glb: c_int,
    );
    fn lua_settop(L: *mut LuaState, idx: c_int);
}

unsafe extern "C" fn l_double(L: *mut LuaState) -> c_int {
    let x = luaL_checkinteger(L, 1);
    lua_pushinteger(L, x * 2);
    1
}

unsafe extern "C" fn luaopen_rustgamelib(L: *mut LuaState) -> c_int {
    lua_createtable(L, 0, 1);
    lua_pushcclosure(L, l_double, 0);
    lua_setfield(L, -2, b"double\0".as_ptr());
    1
}

#[no_mangle]
pub unsafe extern "C" fn cart_lua_modules(L: *mut LuaState) {
    luaL_requiref(L, b"rustgamelib\0".as_ptr(), luaopen_rustgamelib, 1);
    lua_settop(L, -2); // lua_pop(L, 1)
}
"#;

    CartProject::new()
        .rust(rust_game_src)
        .lua(
            r#"
function init()
    local m = require("rustgamelib")
    local result = m.double(21)
    if result == 42 then
        blyt32.debug.print("lua+rust-game ok")
    else
        blyt32.debug.print("lua+rust-game wrong")
    end
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
        String::from_utf8_lossy(&output).contains("lua+rust-game ok"),
        "expected 'lua+rust-game ok' in output, got: {}",
        String::from_utf8_lossy(&output)
    );
}

/// Lua → Rust → C three-language call chain.
///
/// C provides `c_double(x)`; Rust calls it and exposes the result via a Lua module;
/// Lua calls the module and verifies the answer.  This test exercises the
/// `languages: { lua:, c:, rust: }` multi-language declaration path end-to-end.
#[test]
fn lua_rust_c_call_chain() {
    require_sdk();
    require_lua_sdk();
    require_rust_riscv_target();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("lua_rust_c_chain");

    let c_src = r#"
int c_double(int x) { return x * 2; }
"#;

    let rust_game_src = r#"#![no_std]
extern crate blyt;

use core::ffi::{c_int, c_void};

type LuaState = c_void;

extern "C" {
    fn c_double(x: c_int) -> c_int;
    fn lua_createtable(L: *mut LuaState, narr: c_int, nrec: c_int);
    fn lua_pushcclosure(
        L: *mut LuaState,
        f: unsafe extern "C" fn(*mut LuaState) -> c_int,
        n: c_int,
    );
    fn lua_setfield(L: *mut LuaState, idx: c_int, k: *const u8);
    fn lua_pushinteger(L: *mut LuaState, n: c_int);
    fn luaL_checkinteger(L: *mut LuaState, arg: c_int) -> c_int;
    fn luaL_requiref(
        L: *mut LuaState,
        modname: *const u8,
        openf: unsafe extern "C" fn(*mut LuaState) -> c_int,
        glb: c_int,
    );
    fn lua_settop(L: *mut LuaState, idx: c_int);
}

unsafe extern "C" fn l_compute(L: *mut LuaState) -> c_int {
    let x = luaL_checkinteger(L, 1);
    lua_pushinteger(L, c_double(x));
    1
}

unsafe extern "C" fn luaopen_bridge(L: *mut LuaState) -> c_int {
    lua_createtable(L, 0, 1);
    lua_pushcclosure(L, l_compute, 0);
    lua_setfield(L, -2, b"compute\0".as_ptr());
    1
}

#[no_mangle]
pub unsafe extern "C" fn cart_lua_modules(L: *mut LuaState) {
    luaL_requiref(L, b"bridge\0".as_ptr(), luaopen_bridge, 1);
    lua_settop(L, -2);
}
"#;

    CartProject::new()
        .c(c_src)
        .rust(rust_game_src)
        .lua(
            r#"
function init()
    local bridge = require("bridge")
    local result = bridge.compute(21)
    if result == 42 then
        blyt32.debug.print("lua->rust->c ok")
    else
        blyt32.debug.print("lua->rust->c wrong: " .. tostring(result))
    end
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
        String::from_utf8_lossy(&output).contains("lua->rust->c ok"),
        "expected 'lua->rust->c ok' in output, got: {}",
        String::from_utf8_lossy(&output)
    );
}
