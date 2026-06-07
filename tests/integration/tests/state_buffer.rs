mod common;

use common::{
    CartProject, build_cart, build_lua_cart, require_cpp_sdk, require_lua_sdk,
    require_rust_riscv_target, require_sdk, run_cart_native_with_env,
};
use tempfile::TempDir;

const CART_CONFIG: &str = "\
records:
  Game:
    fields:
      - { name: score, type: i32 }
state_buffers:
  game:
    record: Game
    count: 1
";

/// Save/load round-trip for a C cart: write score=42, save, clobber to 99,
/// restore, read back — must print "score=42".
#[test]
fn c_cart_state_buffer_round_trips() {
    require_sdk();

    let tmp = TempDir::new().unwrap();
    let save_dir = TempDir::new().unwrap();
    let project = tmp.path().join("sb_c");

    CartProject::new()
        .config(CART_CONFIG)
        .c(r#"
#include "blyt.h"
#include "cart_state.h"
#include <stdio.h>

void blyt_cart_init(void) {
    int32_t slot = -1;
    blyt_buffer_alloc_slot(S_GAME, &slot);
    blyt_buffer_set_i32(S_GAME, slot, S_GAME_SCORE, 42);
    blyt_save_write(0);
    blyt_buffer_set_i32(S_GAME, slot, S_GAME_SCORE, 99);
}

void blyt_cart_update(void) {
    blyt_save_read(0);
    int32_t score = blyt_buffer_get_i32(S_GAME, 0, S_GAME_SCORE);
    char buf[32];
    snprintf(buf, sizeof(buf), "score=%d", score);
    blyt_console_debug(buf);
    blyt_quit();
}

void blyt_cart_draw(void) {}
"#)
        .write(&project);

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    run_cart_native_with_env(
        &cart,
        &[("BLYT_SAVE_DIR", save_dir.path().to_str().unwrap())],
        "score=42",
    );
}

/// Save/load round-trip for a Rust cart.
#[test]
fn rust_cart_state_buffer_round_trips() {
    require_sdk();
    require_rust_riscv_target();

    let tmp = TempDir::new().unwrap();
    let save_dir = TempDir::new().unwrap();
    let project = tmp.path().join("sb_rust");

    CartProject::new()
        .config(CART_CONFIG)
        .rust(r#"#![no_std]
include!(env!("BLYT_CART_STATE_RS"));

use blyt::buffer::{alloc_slot, get_i32, set_i32};
use blyt::save::{save_read, save_write};

#[no_mangle]
pub extern "C" fn blyt_cart_init() {
    let _slot = alloc_slot(S_GAME);
    set_i32(S_GAME, 0, S_GAME_SCORE, 42);
    save_write(0);
    set_i32(S_GAME, 0, S_GAME_SCORE, 99);
}

#[no_mangle]
pub extern "C" fn blyt_cart_update() {
    save_read(0);
    let score = get_i32(S_GAME, 0, S_GAME_SCORE);
    if score == 42 {
        blyt::console_debug("score=42");
    } else {
        blyt::console_debug("score=wrong");
    }
    blyt::quit();
}

#[no_mangle]
pub extern "C" fn blyt_cart_draw() {}
"#)
        .write(&project);

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    run_cart_native_with_env(
        &cart,
        &[("BLYT_SAVE_DIR", save_dir.path().to_str().unwrap())],
        "score=42",
    );
}

/// Save/load round-trip for a Lua cart.
#[test]
fn lua_cart_state_buffer_round_trips() {
    require_sdk();
    require_lua_sdk();

    let tmp = TempDir::new().unwrap();
    let save_dir = TempDir::new().unwrap();
    let project = tmp.path().join("sb_lua");

    CartProject::new()
        .config(CART_CONFIG)
        .lua(r#"
-- S_GAME and S_GAME_SCORE are globals from cart_state.lua (prepended at build time).
local slot = -1

function init()
    slot = blyt.buf.alloc_slot(S_GAME)
    blyt.buf.set_i32(S_GAME, slot, S_GAME_SCORE, 42)
    blyt.save_write(0)
    blyt.buf.set_i32(S_GAME, slot, S_GAME_SCORE, 99)
end

function update()
    blyt.save_read(0)
    local score = blyt.buf.get_i32(S_GAME, slot, S_GAME_SCORE)
    blyt.debug.print("score=" .. tostring(score))
    blyt.quit()
end

function draw() end
"#)
        .write(&project);

    let cart = build_lua_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    run_cart_native_with_env(
        &cart,
        &[("BLYT_SAVE_DIR", save_dir.path().to_str().unwrap())],
        "score=42",
    );
}

/// Save/load round-trip for a C++ cart.
#[test]
fn cpp_cart_state_buffer_round_trips() {
    require_sdk();
    require_cpp_sdk();

    let tmp = TempDir::new().unwrap();
    let save_dir = TempDir::new().unwrap();
    let project = tmp.path().join("sb_cpp");

    CartProject::new()
        .config(CART_CONFIG)
        .cpp(r#"
#include "blyt.h"
#include "cart_state.h"
#include <cstdio>

extern "C" void blyt_cart_init() {
    int32_t slot = -1;
    blyt_buffer_alloc_slot(S_GAME, &slot);
    blyt_buffer_set_i32(S_GAME, slot, S_GAME_SCORE, 42);
    blyt_save_write(0);
    blyt_buffer_set_i32(S_GAME, slot, S_GAME_SCORE, 99);
}

extern "C" void blyt_cart_update() {
    blyt_save_read(0);
    int32_t score = blyt_buffer_get_i32(S_GAME, 0, S_GAME_SCORE);
    char buf[32];
    snprintf(buf, sizeof(buf), "score=%d", score);
    blyt_console_debug(buf);
    blyt_quit();
}

extern "C" void blyt_cart_draw() {}
"#)
        .write(&project);

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    run_cart_native_with_env(
        &cart,
        &[("BLYT_SAVE_DIR", save_dir.path().to_str().unwrap())],
        "score=42",
    );
}
