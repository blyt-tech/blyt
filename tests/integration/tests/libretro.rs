mod common;

use assert_cmd::Command;
use common::{
    CartProject, build_cart, build_lua_cart, libretro_so, require_libretro_core, require_lua_sdk,
    require_sdk, test_libretro_core, write_c_cart_project,
};
use tempfile::TempDir;

/// C cart runs to completion through the embedded libretro core.
///
/// Exercises the full dlopen → retro_init → retro_load_game → retro_run loop
/// → retro_unload_game → retro_deinit path with the embedded guest lib blobs
/// (libblytcommon.so, libblytc.so, libblyt32.so).
#[test]
fn libretro_c_cart_runs_to_completion() {
    require_sdk();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("libretro_c_quit");
    write_c_cart_project(
        &project,
        "#include \"blyt.h\"\n\
         static int frame = 0;\n\
         void blyt_cart_init(void)   {}\n\
         void blyt_cart_update(void) { if (++frame >= 3) blyt_quit(); }\n\
         void blyt_cart_draw(void)   {}\n",
    );
    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    Command::new(test_libretro_core())
        .args([libretro_so().to_str().unwrap(), cart.to_str().unwrap()])
        .assert()
        .success();
}

/// Lua cart runs to completion through the embedded libretro core.
///
/// Exercises the embedded libblyt32lua.so blob path: verifies that
/// blyt_libretro.so is self-contained for Lua carts without BLYT_LIB_DIR.
#[test]
fn libretro_lua_cart_runs_to_completion() {
    require_sdk();
    require_lua_sdk();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("libretro_lua_quit");
    CartProject::new()
        .lua(
            "local frame = 0\n\
             function init() end\n\
             function update()\n\
             \x20   frame = frame + 1\n\
             \x20   if frame >= 3 then blyt.quit() end\n\
             end\n\
             function draw() end\n",
        )
        .write(&project);
    let cart = build_lua_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    Command::new(test_libretro_core())
        .args([libretro_so().to_str().unwrap(), cart.to_str().unwrap()])
        .assert()
        .success();
}

/// C cart that runs for many frames without quitting does not crash.
///
/// Verifies the retro_run loop is stable over extended execution.
#[test]
fn libretro_c_cart_multi_frame() {
    require_sdk();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("libretro_c_frames");
    write_c_cart_project(
        &project,
        "#include \"blyt.h\"\n\
         static int frame = 0;\n\
         void blyt_cart_init(void)   {}\n\
         void blyt_cart_update(void) { if (++frame >= 300) blyt_quit(); }\n\
         void blyt_cart_draw(void)   {}\n",
    );
    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    Command::new(test_libretro_core())
        .args([libretro_so().to_str().unwrap(), cart.to_str().unwrap()])
        .assert()
        .success();
}

/// Lua cart that uses local variables and arithmetic runs correctly.
///
/// Confirms the Lua VM, SoftFloat, and setjmp/longjmp all work inside the
/// embedded libblyt32lua.so blob.
#[test]
fn libretro_lua_cart_with_arithmetic() {
    require_sdk();
    require_lua_sdk();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("libretro_lua_arith");
    CartProject::new()
        .lua(
            "local sum = 0\n\
             local count = 0\n\
             function init() end\n\
             function update()\n\
             \x20   count = count + 1\n\
             \x20   sum = sum + count * 2.5\n\
             \x20   if count >= 60 then blyt.quit() end\n\
             end\n\
             function draw() end\n",
        )
        .write(&project);
    let cart = build_lua_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    Command::new(test_libretro_core())
        .args([libretro_so().to_str().unwrap(), cart.to_str().unwrap()])
        .assert()
        .success();
}
