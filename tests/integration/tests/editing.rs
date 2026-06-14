//! Issue #48 item 1: editor config emission (clangd + LuaLS).
//!
//! `blyt build` emits a clangd compile database for C/C++ carts and LuaLS
//! annotations for the `S` state proxy, so go-to-definition / completion work
//! while editing.  (The rust-analyzer + .luarc.json registration are covered in
//! the setup suite.)

mod common;

use common::{CartProject, build_cart, build_lua_cart, require_sdk};
use tempfile::TempDir;

const TINY_C: &str = r#"
#include "blyt.h"
void blyt_cart_init(void)   { blyt_console_debug("editing"); }
void blyt_cart_update(void) { blyt_quit(); }
void blyt_cart_draw(void)   {}
"#;

/// A C cart gets a compile_commands.json clangd can load, with the cross-compile
/// target and the source file.
#[test]
fn build_emits_compile_commands() {
    require_sdk();
    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("c_cart");
    CartProject::new().c(TINY_C).write(&project);
    build_cart(&project);

    let db = project.join("build/compile_commands.json");
    let json = std::fs::read_to_string(&db).expect("compile_commands.json written");
    assert!(json.contains("--target=riscv32"), "missing target:\n{json}");
    assert!(
        json.contains("src/game/c") && json.contains("\"arguments\""),
        "compile_commands.json missing file/arguments:\n{json}"
    );
    // Structurally a non-empty JSON array of entries.
    assert!(
        json.trim_start().starts_with('[') && json.contains("\"directory\""),
        "compile_commands.json not a JSON array of entries:\n{json}"
    );
}

const BUFFER_CONFIG: &str = "\
records:
  Player:
    fields:
      - { name: hp, type: i32 }
      - { name: vx, type: f32 }
      - { name: alive, type: bool }
state_buffers:
  player:
    record: Player
    count: 8
";

/// A Lua cart with state buffers gets LuaLS annotations for the `S` proxy:
/// a typed row class, a buffer class with count, and the global `S`.
#[test]
fn build_emits_lua_state_decls() {
    require_sdk();
    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("lua_cart");
    CartProject::new()
        .config(BUFFER_CONFIG)
        .lua("function blyt_cart_init() end\nfunction blyt_cart_update() blyt.quit() end\nfunction blyt_cart_draw() end\n")
        .write(&project);
    build_lua_cart(&project);

    let decls = std::fs::read_to_string(project.join("build/blyt/lua/cart_state.lua"))
        .expect("cart_state.lua written");
    assert!(
        decls.contains("---@meta"),
        "not a LuaLS meta file:\n{decls}"
    );
    assert!(
        decls.contains("---@class blyt.S.player.row"),
        "missing row class:\n{decls}"
    );
    // Field types: i32 -> integer, f32 -> number, bool -> boolean.
    assert!(decls.contains("---@field hp integer"), "hp:\n{decls}");
    assert!(decls.contains("---@field vx number"), "vx:\n{decls}");
    assert!(decls.contains("---@field alive boolean"), "alive:\n{decls}");
    assert!(
        decls.contains("---@field [integer] blyt.S.player.row"),
        "missing indexed rows:\n{decls}"
    );
    assert!(
        decls.contains("---@field player blyt.S.player"),
        "missing S.player:\n{decls}"
    );
}
