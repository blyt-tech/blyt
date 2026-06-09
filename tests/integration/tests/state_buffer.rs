mod common;

use common::{
    CartProject, build_cart, build_lua_cart, require_cpp_sdk, require_lua_sdk,
    require_rust_riscv_target, require_sdk, require_wasm, run_cart_native_with_env,
    run_cart_native_with_flags, run_cart_wasm, run_cart_wasm_with_env,
};
use tempfile::TempDir;

// ── Config helpers ─────────────────────────────────────────────────────────

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

const MULTI_FIELD_CONFIG: &str = "\
records:
  Player:
    fields:
      - { name: score,  type: i32 }
      - { name: health, type: i32 }
      - { name: level,  type: i32 }
state_buffers:
  player:
    record: Player
    count: 1
";

const ALL_TYPES_CONFIG: &str = "\
records:
  Types:
    fields:
      - { name: v_f32,  type: f32  }
      - { name: v_u32,  type: u32  }
      - { name: v_i8,   type: i8   }
      - { name: v_u8,   type: u8   }
      - { name: v_i16,  type: i16  }
      - { name: v_u16,  type: u16  }
      - { name: v_bool, type: bool }
state_buffers:
  types:
    record: Types
    count: 1
";

const MULTI_BUFFER_CONFIG: &str = "\
records:
  Obj:
    fields:
      - { name: x, type: i32 }
  Sfx:
    fields:
      - { name: vol, type: i32 }
state_buffers:
  obj:
    record: Obj
    count: 1
  sfx:
    record: Sfx
    count: 1
";

const MULTI_SLOT_CONFIG: &str = "\
records:
  Entity:
    fields:
      - { name: hp, type: i32 }
state_buffers:
  entity:
    record: Entity
    count: 4
";

// ── Existing gate tests (one per language) ─────────────────────────────────

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
        .rust(
            r#"#![no_std]
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
"#,
        )
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
        .lua(
            r#"
-- S.game is the proxy table; S.game[slot].score reads/writes via blyt.buf.
local slot = -1

function init()
    slot = blyt.buf.alloc_slot(S.GAME)
    S.game[slot].score = 42
    blyt.save_write(0)
    S.game[slot].score = 99
end

function update()
    blyt.save_read(0)
    local score = S.game[slot].score
    blyt.debug.print("score=" .. tostring(score))
    blyt.quit()
end

function draw() end
"#,
        )
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
        .cpp(
            r#"
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
"#,
        )
        .write(&project);

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    run_cart_native_with_env(
        &cart,
        &[("BLYT_SAVE_DIR", save_dir.path().to_str().unwrap())],
        "score=42",
    );
}

// ── Type coverage ──────────────────────────────────────────────────────────

/// All seven field types survive a save/load round-trip.
#[test]
fn c_cart_all_field_types_round_trips() {
    require_sdk();

    let tmp = TempDir::new().unwrap();
    let save_dir = TempDir::new().unwrap();
    let project = tmp.path().join("sb_types");

    CartProject::new()
        .config(ALL_TYPES_CONFIG)
        .c(r#"
#include "blyt.h"
#include "cart_state.h"
#include <math.h>
#include <stdio.h>

void blyt_cart_init(void) {
    int32_t slot = -1;
    blyt_buffer_alloc_slot(S_TYPES, &slot);
    blyt_buffer_set_f32 (S_TYPES, slot, S_TYPES_V_F32,  1.5f);
    blyt_buffer_set_u32 (S_TYPES, slot, S_TYPES_V_U32,  0xDEADBEEFu);
    blyt_buffer_set_i8  (S_TYPES, slot, S_TYPES_V_I8,   -42);
    blyt_buffer_set_u8  (S_TYPES, slot, S_TYPES_V_U8,   200);
    blyt_buffer_set_i16 (S_TYPES, slot, S_TYPES_V_I16,  -1000);
    blyt_buffer_set_u16 (S_TYPES, slot, S_TYPES_V_U16,  60000);
    blyt_buffer_set_bool(S_TYPES, slot, S_TYPES_V_BOOL, 1);
    blyt_save_write(0);
    blyt_buffer_set_f32 (S_TYPES, slot, S_TYPES_V_F32,  0.0f);
    blyt_buffer_set_u32 (S_TYPES, slot, S_TYPES_V_U32,  0);
    blyt_buffer_set_i8  (S_TYPES, slot, S_TYPES_V_I8,   0);
    blyt_buffer_set_u8  (S_TYPES, slot, S_TYPES_V_U8,   0);
    blyt_buffer_set_i16 (S_TYPES, slot, S_TYPES_V_I16,  0);
    blyt_buffer_set_u16 (S_TYPES, slot, S_TYPES_V_U16,  0);
    blyt_buffer_set_bool(S_TYPES, slot, S_TYPES_V_BOOL, 0);
}

void blyt_cart_update(void) {
    blyt_save_read(0);
    float    vf  = blyt_buffer_get_f32 (S_TYPES, 0, S_TYPES_V_F32);
    uint32_t vu  = blyt_buffer_get_u32 (S_TYPES, 0, S_TYPES_V_U32);
    int8_t   vi8 = blyt_buffer_get_i8  (S_TYPES, 0, S_TYPES_V_I8);
    uint8_t  vu8 = blyt_buffer_get_u8  (S_TYPES, 0, S_TYPES_V_U8);
    int16_t  vi16 = blyt_buffer_get_i16(S_TYPES, 0, S_TYPES_V_I16);
    uint16_t vu16 = blyt_buffer_get_u16(S_TYPES, 0, S_TYPES_V_U16);
    int      vb  = blyt_buffer_get_bool(S_TYPES, 0, S_TYPES_V_BOOL) ? 1 : 0;
    if (vf == 1.5f && vu == 0xDEADBEEFu && vi8 == -42 && vu8 == 200
            && vi16 == -1000 && vu16 == 60000 && vb)
        blyt_console_debug("types_ok");
    else
        blyt_console_debug("types_fail");
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
        "types_ok",
    );
}

// ── Multiple fields ────────────────────────────────────────────────────────

/// Multiple fields in one record all survive a round-trip.
#[test]
fn c_cart_multiple_fields_round_trips() {
    require_sdk();

    let tmp = TempDir::new().unwrap();
    let save_dir = TempDir::new().unwrap();
    let project = tmp.path().join("sb_fields");

    CartProject::new()
        .config(MULTI_FIELD_CONFIG)
        .c(r#"
#include "blyt.h"
#include "cart_state.h"
#include <stdio.h>

void blyt_cart_init(void) {
    int32_t slot = -1;
    blyt_buffer_alloc_slot(S_PLAYER, &slot);
    blyt_buffer_set_i32(S_PLAYER, slot, S_PLAYER_SCORE,  1000);
    blyt_buffer_set_i32(S_PLAYER, slot, S_PLAYER_HEALTH, 75);
    blyt_buffer_set_i32(S_PLAYER, slot, S_PLAYER_LEVEL,  5);
    blyt_save_write(0);
    blyt_buffer_set_i32(S_PLAYER, slot, S_PLAYER_SCORE,  0);
    blyt_buffer_set_i32(S_PLAYER, slot, S_PLAYER_HEALTH, 0);
    blyt_buffer_set_i32(S_PLAYER, slot, S_PLAYER_LEVEL,  0);
}

void blyt_cart_update(void) {
    blyt_save_read(0);
    int32_t s = blyt_buffer_get_i32(S_PLAYER, 0, S_PLAYER_SCORE);
    int32_t h = blyt_buffer_get_i32(S_PLAYER, 0, S_PLAYER_HEALTH);
    int32_t l = blyt_buffer_get_i32(S_PLAYER, 0, S_PLAYER_LEVEL);
    if (s == 1000 && h == 75 && l == 5)
        blyt_console_debug("fields_ok");
    else
        blyt_console_debug("fields_fail");
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
        "fields_ok",
    );
}

// ── Multiple buffers ───────────────────────────────────────────────────────

/// Values in two independent state buffers both survive a round-trip.
#[test]
fn c_cart_multiple_buffers_round_trips() {
    require_sdk();

    let tmp = TempDir::new().unwrap();
    let save_dir = TempDir::new().unwrap();
    let project = tmp.path().join("sb_multibuf");

    CartProject::new()
        .config(MULTI_BUFFER_CONFIG)
        .c(r#"
#include "blyt.h"
#include "cart_state.h"

void blyt_cart_init(void) {
    int32_t s1 = -1, s2 = -1;
    blyt_buffer_alloc_slot(S_OBJ, &s1);
    blyt_buffer_alloc_slot(S_SFX, &s2);
    blyt_buffer_set_i32(S_OBJ, s1, S_OBJ_X,   99);
    blyt_buffer_set_i32(S_SFX, s2, S_SFX_VOL, 77);
    blyt_save_write(0);
    blyt_buffer_set_i32(S_OBJ, s1, S_OBJ_X,   0);
    blyt_buffer_set_i32(S_SFX, s2, S_SFX_VOL, 0);
}

void blyt_cart_update(void) {
    blyt_save_read(0);
    int32_t x   = blyt_buffer_get_i32(S_OBJ, 0, S_OBJ_X);
    int32_t vol = blyt_buffer_get_i32(S_SFX, 0, S_SFX_VOL);
    if (x == 99 && vol == 77)
        blyt_console_debug("buffers_ok");
    else
        blyt_console_debug("buffers_fail");
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
        "buffers_ok",
    );
}

// ── Multiple slots ─────────────────────────────────────────────────────────

/// Values written to 4 distinct slots all survive a round-trip (native).
#[test]
fn c_cart_multiple_slots_round_trips() {
    require_sdk();

    let tmp = TempDir::new().unwrap();
    let save_dir = TempDir::new().unwrap();
    let project = tmp.path().join("sb_slots");

    CartProject::new()
        .config(MULTI_SLOT_CONFIG)
        .c(r#"
#include "blyt.h"
#include "cart_state.h"

void blyt_cart_init(void) {
    for (int32_t i = 0; i < 4; i++) {
        int32_t slot = -1;
        blyt_buffer_alloc_slot(S_ENTITY, &slot);
        blyt_buffer_set_i32(S_ENTITY, slot, S_ENTITY_HP, (i + 1) * 10);
    }
    blyt_save_write(0);
    for (int32_t i = 0; i < 4; i++)
        blyt_buffer_set_i32(S_ENTITY, i, S_ENTITY_HP, 0);
}

void blyt_cart_update(void) {
    blyt_save_read(0);
    int ok = 1;
    for (int32_t i = 0; i < 4; i++) {
        int32_t expected = (i + 1) * 10;
        if (blyt_buffer_get_i32(S_ENTITY, i, S_ENTITY_HP) != expected) {
            ok = 0;
            break;
        }
    }
    blyt_console_debug(ok ? "slots_ok" : "slots_fail");
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
        "slots_ok",
    );
}

/// Same multi-slot test on the WASM target.
#[test]
fn wasm_c_cart_multiple_slots_round_trips() {
    require_sdk();
    require_wasm();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("wsb_slots");

    CartProject::new()
        .config(MULTI_SLOT_CONFIG)
        .c(r#"
#include "blyt.h"
#include "cart_state.h"

void blyt_cart_init(void) {
    for (int32_t i = 0; i < 4; i++) {
        int32_t slot = -1;
        blyt_buffer_alloc_slot(S_ENTITY, &slot);
        blyt_buffer_set_i32(S_ENTITY, slot, S_ENTITY_HP, (i + 1) * 10);
    }
    blyt_save_write(0);
    for (int32_t i = 0; i < 4; i++)
        blyt_buffer_set_i32(S_ENTITY, i, S_ENTITY_HP, 0);
}

void blyt_cart_update(void) {
    blyt_save_read(0);
    int ok = 1;
    for (int32_t i = 0; i < 4; i++) {
        int32_t expected = (i + 1) * 10;
        if (blyt_buffer_get_i32(S_ENTITY, i, S_ENTITY_HP) != expected) {
            ok = 0;
            break;
        }
    }
    blyt_console_debug(ok ? "slots_ok" : "slots_fail");
    blyt_quit();
}

void blyt_cart_draw(void) {}
"#)
        .write(&project);

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());
    run_cart_wasm_with_env(&cart, &[("BLYT_SAVE_DIR", "/tmp")], "slots_ok");
}

// ── Free slot ──────────────────────────────────────────────────────────────

/// blyt_buffer_free_slot recycles the slot for the next alloc.
#[test]
fn c_cart_free_slot() {
    require_sdk();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("sb_free");

    CartProject::new()
        .config(MULTI_SLOT_CONFIG)
        .c(r#"
#include "blyt.h"
#include "cart_state.h"

void blyt_cart_init(void) {}

void blyt_cart_update(void) {
    int32_t s0 = -1;
    blyt_buffer_alloc_slot(S_ENTITY, &s0);
    blyt_buffer_set_i32(S_ENTITY, s0, S_ENTITY_HP, 42);
    blyt_buffer_free_slot(S_ENTITY, s0);

    int32_t s1 = -1;
    blyt_buffer_alloc_slot(S_ENTITY, &s1);
    blyt_buffer_set_i32(S_ENTITY, s1, S_ENTITY_HP, 99);

    int32_t got = blyt_buffer_get_i32(S_ENTITY, s1, S_ENTITY_HP);
    blyt_console_debug(got == 99 ? "free_ok" : "free_fail");
    blyt_quit();
}

void blyt_cart_draw(void) {}
"#)
        .write(&project);

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());
    common::run_cart_native(&cart, "free_ok");
}

// ── Multiple save slots ────────────────────────────────────────────────────

/// Save to slot 0 and slot 1 independently; loads return the correct values.
#[test]
fn c_cart_multiple_save_slots() {
    require_sdk();

    let tmp = TempDir::new().unwrap();
    let save_dir = TempDir::new().unwrap();
    let project = tmp.path().join("sb_saveslots");

    CartProject::new()
        .config(CART_CONFIG)
        .c(r#"
#include "blyt.h"
#include "cart_state.h"
#include <stdio.h>

static int s_frame = 0;

void blyt_cart_init(void) {
    int32_t slot = -1;
    blyt_buffer_alloc_slot(S_GAME, &slot);
    blyt_buffer_set_i32(S_GAME, slot, S_GAME_SCORE, 11);
    blyt_save_write(0);
    blyt_buffer_set_i32(S_GAME, slot, S_GAME_SCORE, 22);
    blyt_save_write(1);
}

void blyt_cart_update(void) {
    char buf[32];
    if (s_frame == 0) {
        blyt_save_read(0);
        int32_t v = blyt_buffer_get_i32(S_GAME, 0, S_GAME_SCORE);
        snprintf(buf, sizeof(buf), "s0=%d", v);
        blyt_console_debug(buf);
    } else {
        blyt_save_read(1);
        int32_t v = blyt_buffer_get_i32(S_GAME, 0, S_GAME_SCORE);
        snprintf(buf, sizeof(buf), "s1=%d", v);
        blyt_console_debug(buf);
        blyt_quit();
    }
    s_frame++;
}

void blyt_cart_draw(void) {}
"#)
        .write(&project);

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let save_str = save_dir.path().to_str().unwrap();
    run_cart_native_with_env(&cart, &[("BLYT_SAVE_DIR", save_str)], "s0=11");
    run_cart_native_with_env(&cart, &[("BLYT_SAVE_DIR", save_str)], "s1=22");
}

// ── Schema mismatch ────────────────────────────────────────────────────────

/// When the saved schema hash differs from the current schema, blyt_save_read
/// returns a non-OK result and the cart handles it gracefully without crashing.
#[test]
fn c_cart_schema_mismatch_graceful() {
    require_sdk();

    let save_dir = TempDir::new().unwrap();
    let save_str = save_dir.path().to_str().unwrap();

    // v1 cart: schema {score: i32} — writes save file.
    let tmp_v1 = TempDir::new().unwrap();
    let project_v1 = tmp_v1.path().join("sb_migrate");

    CartProject::new()
        .config(
            "records:\n  V1:\n    fields:\n      - { name: score, type: i32 }\n\
             state_buffers:\n  v1:\n    record: V1\n    count: 1\n",
        )
        .c(r#"
#include "blyt.h"
#include "cart_state.h"

void blyt_cart_init(void) {
    int32_t slot = -1;
    blyt_buffer_alloc_slot(S_V1, &slot);
    blyt_buffer_set_i32(S_V1, slot, S_V1_SCORE, 55);
    blyt_save_write(0);
}

void blyt_cart_update(void) { blyt_quit(); }
void blyt_cart_draw(void)   {}
"#)
        .write(&project_v1);

    let cart_v1 = build_cart(&project_v1);
    assert!(cart_v1.exists());
    run_cart_native_with_env(&cart_v1, &[("BLYT_SAVE_DIR", save_str)], "");

    // v2 cart: schema {score: i32, health: i32} — different hash, load must
    // fail gracefully and the cart prints "load_failed" to confirm the return
    // value was checked.
    let tmp_v2 = TempDir::new().unwrap();
    let project_v2 = tmp_v2.path().join("sb_migrate");

    CartProject::new()
        .config(concat!(
            "records:\n  V1:\n    fields:\n",
            "      - { name: score, type: i32 }\n",
            "      - { name: health, type: i32 }\n",
            "state_buffers:\n  v1:\n    record: V1\n    count: 1\n",
        ))
        .c(r#"
#include "blyt.h"
#include "cart_state.h"

void blyt_cart_init(void) {
    int32_t slot = -1;
    blyt_buffer_alloc_slot(S_V1, &slot);
}

void blyt_cart_update(void) {
    blyt_result_t r = blyt_save_read(0);
    blyt_console_debug(r == BLYT_OK ? "load_ok" : "load_failed");
    blyt_quit();
}

void blyt_cart_draw(void) {}
"#)
        .write(&project_v2);

    let cart_v2 = build_cart(&project_v2);
    assert!(cart_v2.exists());
    run_cart_native_with_env(&cart_v2, &[("BLYT_SAVE_DIR", save_str)], "load_failed");
}

// ── NaN canonicalization ───────────────────────────────────────────────────

/// Writing a signaling NaN to an f32 field stores the canonical quiet NaN
/// (ADR-0010: NaN canonicalization on all f32 writes).
#[test]
fn c_cart_nan_canonicalization() {
    require_sdk();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("sb_nan");

    CartProject::new()
        .config(ALL_TYPES_CONFIG)
        .c(r#"
#include "blyt.h"
#include "cart_state.h"
#include <stdint.h>
#include <string.h>

/* Canonical quiet NaN per ADR-0010. */
#define CANONICAL_NAN_BITS 0x7FC00000u

void blyt_cart_init(void) {
    int32_t slot = -1;
    blyt_buffer_alloc_slot(S_TYPES, &slot);
    /* Write a signaling NaN: exponent all-1s, mantissa 0x200001. */
    float snan;
    uint32_t snan_bits = 0x7F800001u;
    memcpy(&snan, &snan_bits, 4);
    blyt_buffer_set_f32(S_TYPES, slot, S_TYPES_V_F32, snan);
}

void blyt_cart_update(void) {
    float v = blyt_buffer_get_f32(S_TYPES, 0, S_TYPES_V_F32);
    uint32_t bits;
    memcpy(&bits, &v, 4);
    blyt_console_debug(bits == CANONICAL_NAN_BITS ? "nan_ok" : "nan_fail");
    blyt_quit();
}

void blyt_cart_draw(void) {}
"#)
        .write(&project);

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());
    common::run_cart_native(&cart, "nan_ok");
}

// ── Lifecycle callbacks ────────────────────────────────────────────────────

/// blyt_cart_on_new_state is called once between init and the first update.
#[test]
fn c_cart_lifecycle_on_new_state() {
    require_sdk();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("sb_lifecycle_new");

    CartProject::new()
        .config(CART_CONFIG)
        .c(r#"
#include "blyt.h"
#include "cart_state.h"

void blyt_cart_on_new_state(void) {
    blyt_console_debug("new_state_called");
}

void blyt_cart_init(void)   {}
void blyt_cart_update(void) { blyt_quit(); }
void blyt_cart_draw(void)   {}
"#)
        .write(&project);

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());
    common::run_cart_native(&cart, "new_state_called");
}

/// blyt_cart_on_save_state fires during blyt_save_write; blyt_cart_on_load_state
/// fires after a successful blyt_save_read.
#[test]
fn c_cart_lifecycle_on_save_load_state() {
    require_sdk();

    let tmp = TempDir::new().unwrap();
    let save_dir = TempDir::new().unwrap();
    let project = tmp.path().join("sb_lifecycle_sl");

    CartProject::new()
        .config(CART_CONFIG)
        .c(r#"
#include "blyt.h"
#include "cart_state.h"

void blyt_cart_on_save_state(void) {
    blyt_console_debug("save_state_called");
}

void blyt_cart_on_load_state(blyt_load_info_t info) {
    (void)info;
    blyt_console_debug("load_state_called");
}

void blyt_cart_init(void) {
    int32_t slot = -1;
    blyt_buffer_alloc_slot(S_GAME, &slot);
    blyt_buffer_set_i32(S_GAME, slot, S_GAME_SCORE, 7);
    blyt_save_write(0);
}

void blyt_cart_update(void) {
    blyt_save_read(0);
    blyt_quit();
}

void blyt_cart_draw(void) {}
"#)
        .write(&project);

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let save_str = save_dir.path().to_str().unwrap();
    run_cart_native_with_env(&cart, &[("BLYT_SAVE_DIR", save_str)], "save_state_called");
    run_cart_native_with_env(&cart, &[("BLYT_SAVE_DIR", save_str)], "load_state_called");
}

// ── Lua proxy ──────────────────────────────────────────────────────────────

/// S.game.count returns the declared buffer count.
#[test]
fn lua_cart_proxy_count() {
    require_sdk();
    require_lua_sdk();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("sb_lua_count");

    CartProject::new()
        .config(MULTI_SLOT_CONFIG)
        .lua(
            r#"
function init()
    local c = S.entity.count
    blyt.debug.print("count=" .. tostring(c))
end
function update() blyt.quit() end
function draw() end
"#,
        )
        .write(&project);

    let cart = build_lua_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());
    common::run_cart_native(&cart, "count=4");
}

/// Proxy write + read across 4 slots using S.entity[i].hp syntax.
#[test]
fn lua_cart_proxy_multiple_slots() {
    require_sdk();
    require_lua_sdk();

    let tmp = TempDir::new().unwrap();
    let save_dir = TempDir::new().unwrap();
    let project = tmp.path().join("sb_lua_slots");

    CartProject::new()
        .config(MULTI_SLOT_CONFIG)
        .lua(
            r#"
function init()
    for i = 0, 3 do
        blyt.buf.alloc_slot(S.ENTITY)
        S.entity[i].hp = (i + 1) * 10
    end
    blyt.save_write(0)
    for i = 0, 3 do
        S.entity[i].hp = 0
    end
end

function update()
    blyt.save_read(0)
    local ok = true
    for i = 0, 3 do
        if S.entity[i].hp ~= (i + 1) * 10 then
            ok = false
            break
        end
    end
    blyt.debug.print(ok and "lua_slots_ok" or "lua_slots_fail")
    blyt.quit()
end

function draw() end
"#,
        )
        .write(&project);

    let cart = build_lua_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());
    run_cart_native_with_env(
        &cart,
        &[("BLYT_SAVE_DIR", save_dir.path().to_str().unwrap())],
        "lua_slots_ok",
    );
}

// ── WASM tests ─────────────────────────────────────────────────────────────

/// Basic save/load round-trip on the WASM target (C cart, single i32 field).
#[test]
fn wasm_c_cart_state_buffer_round_trips() {
    require_sdk();
    require_wasm();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("wsb_c");

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
    run_cart_wasm_with_env(&cart, &[("BLYT_SAVE_DIR", "/tmp")], "score=42");
}

/// All field types survive a save/load round-trip on the WASM target.
#[test]
fn wasm_c_cart_all_field_types_round_trips() {
    require_sdk();
    require_wasm();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("wsb_types");

    CartProject::new()
        .config(ALL_TYPES_CONFIG)
        .c(r#"
#include "blyt.h"
#include "cart_state.h"
#include <stdio.h>

void blyt_cart_init(void) {
    int32_t slot = -1;
    blyt_buffer_alloc_slot(S_TYPES, &slot);
    blyt_buffer_set_f32 (S_TYPES, slot, S_TYPES_V_F32,  1.5f);
    blyt_buffer_set_u32 (S_TYPES, slot, S_TYPES_V_U32,  0xDEADBEEFu);
    blyt_buffer_set_i8  (S_TYPES, slot, S_TYPES_V_I8,   -42);
    blyt_buffer_set_u8  (S_TYPES, slot, S_TYPES_V_U8,   200);
    blyt_buffer_set_i16 (S_TYPES, slot, S_TYPES_V_I16,  -1000);
    blyt_buffer_set_u16 (S_TYPES, slot, S_TYPES_V_U16,  60000);
    blyt_buffer_set_bool(S_TYPES, slot, S_TYPES_V_BOOL, 1);
    blyt_save_write(0);
    blyt_buffer_set_f32 (S_TYPES, slot, S_TYPES_V_F32,  0.0f);
    blyt_buffer_set_u32 (S_TYPES, slot, S_TYPES_V_U32,  0);
    blyt_buffer_set_i8  (S_TYPES, slot, S_TYPES_V_I8,   0);
    blyt_buffer_set_u8  (S_TYPES, slot, S_TYPES_V_U8,   0);
    blyt_buffer_set_i16 (S_TYPES, slot, S_TYPES_V_I16,  0);
    blyt_buffer_set_u16 (S_TYPES, slot, S_TYPES_V_U16,  0);
    blyt_buffer_set_bool(S_TYPES, slot, S_TYPES_V_BOOL, 0);
}

void blyt_cart_update(void) {
    blyt_save_read(0);
    float    vf   = blyt_buffer_get_f32 (S_TYPES, 0, S_TYPES_V_F32);
    uint32_t vu   = blyt_buffer_get_u32 (S_TYPES, 0, S_TYPES_V_U32);
    int8_t   vi8  = blyt_buffer_get_i8  (S_TYPES, 0, S_TYPES_V_I8);
    uint8_t  vu8  = blyt_buffer_get_u8  (S_TYPES, 0, S_TYPES_V_U8);
    int16_t  vi16 = blyt_buffer_get_i16 (S_TYPES, 0, S_TYPES_V_I16);
    uint16_t vu16 = blyt_buffer_get_u16 (S_TYPES, 0, S_TYPES_V_U16);
    int      vb   = blyt_buffer_get_bool(S_TYPES, 0, S_TYPES_V_BOOL) ? 1 : 0;
    if (vf == 1.5f && vu == 0xDEADBEEFu && vi8 == -42 && vu8 == 200
            && vi16 == -1000 && vu16 == 60000 && vb)
        blyt_console_debug("types_ok");
    else
        blyt_console_debug("types_fail");
    blyt_quit();
}

void blyt_cart_draw(void) {}
"#)
        .write(&project);

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());
    run_cart_wasm_with_env(&cart, &[("BLYT_SAVE_DIR", "/tmp")], "types_ok");
}

// ── Lua lifecycle callbacks ────────────────────────────────────────────────

/// blyt_cart_on_new_state is dispatched to the Lua `on_new_state` global.
/// Without the dispatch in blyt32lua.c (or the sym export), the callback is
/// silently swallowed — this test catches that regression.
#[test]
fn lua_cart_lifecycle_on_new_state() {
    require_sdk();
    require_lua_sdk();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("lua_lifecycle_new");

    CartProject::new()
        .lua(
            r#"
function on_new_state()
    blyt.debug.print("new_state_called")
end

function init()   end
function update() blyt.quit() end
function draw()   end
"#,
        )
        .write(&project);

    let cart = build_lua_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());
    common::run_cart_native(&cart, "new_state_called");
}

/// blyt_cart_on_save_state and blyt_cart_on_load_state are dispatched to
/// the Lua `on_save_state` / `on_load_state` globals.
#[test]
fn lua_cart_lifecycle_on_save_load_state() {
    require_sdk();
    require_lua_sdk();

    let tmp = TempDir::new().unwrap();
    let save_dir = TempDir::new().unwrap();
    let project = tmp.path().join("lua_lifecycle_sl");

    CartProject::new()
        .config(CART_CONFIG)
        .lua(
            r#"
local slot = -1

function on_save_state()
    blyt.debug.print("save_called")
end

function on_load_state(info)
    blyt.debug.print("load_called")
end

function init()
    slot = blyt.buf.alloc_slot(S.GAME)
    blyt.save_write(0)
end

function update()
    blyt.save_read(0)
    blyt.quit()
end

function draw() end
"#,
        )
        .write(&project);

    let cart = build_lua_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let save_str = save_dir.path().to_str().unwrap();
    run_cart_native_with_env(&cart, &[("BLYT_SAVE_DIR", save_str)], "save_called");
    run_cart_native_with_env(&cart, &[("BLYT_SAVE_DIR", save_str)], "load_called");
}

// ── Rust lifecycle callbacks ───────────────────────────────────────────────

/// blyt_cart_on_new_state fires for Rust carts.
#[test]
fn rust_cart_lifecycle_on_new_state() {
    require_sdk();
    require_rust_riscv_target();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("rust_lifecycle_new");

    CartProject::new()
        .rust(
            r#"#![no_std]

#[no_mangle]
pub extern "C" fn blyt_cart_on_new_state() {
    blyt::console_debug("new_state_called");
}

#[no_mangle]
pub extern "C" fn blyt_cart_init() {}

#[no_mangle]
pub extern "C" fn blyt_cart_update() { blyt::quit(); }

#[no_mangle]
pub extern "C" fn blyt_cart_draw() {}
"#,
        )
        .write(&project);

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());
    common::run_cart_native(&cart, "new_state_called");
}

/// blyt_cart_on_save_state and blyt_cart_on_load_state fire for Rust carts.
#[test]
fn rust_cart_lifecycle_on_save_load_state() {
    require_sdk();
    require_rust_riscv_target();

    let tmp = TempDir::new().unwrap();
    let save_dir = TempDir::new().unwrap();
    let project = tmp.path().join("rust_lifecycle_sl");

    CartProject::new()
        .config(CART_CONFIG)
        .rust(
            r#"#![no_std]
include!(env!("BLYT_CART_STATE_RS"));

use blyt::buffer::alloc_slot;
use blyt::save::{save_read, save_write};

#[no_mangle]
pub extern "C" fn blyt_cart_on_save_state() {
    blyt::console_debug("save_called");
}

#[repr(C)]
pub struct BlytLoadInfo { _reason: u32, _version: u32, _buffers: u32 }

#[no_mangle]
pub extern "C" fn blyt_cart_on_load_state(_info: BlytLoadInfo) {
    blyt::console_debug("load_called");
}

#[no_mangle]
pub extern "C" fn blyt_cart_init() {
    let _slot = alloc_slot(S_GAME);
    save_write(0);
}

#[no_mangle]
pub extern "C" fn blyt_cart_update() {
    save_read(0);
    blyt::quit();
}

#[no_mangle]
pub extern "C" fn blyt_cart_draw() {}
"#,
        )
        .write(&project);

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let save_str = save_dir.path().to_str().unwrap();
    run_cart_native_with_env(&cart, &[("BLYT_SAVE_DIR", save_str)], "save_called");
    run_cart_native_with_env(&cart, &[("BLYT_SAVE_DIR", save_str)], "load_called");
}

// ── C++ lifecycle callbacks ────────────────────────────────────────────────

/// blyt_cart_on_new_state fires for C++ carts.
#[test]
fn cpp_cart_lifecycle_on_new_state() {
    require_sdk();
    require_cpp_sdk();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("cpp_lifecycle_new");

    CartProject::new()
        .cpp(
            r#"
#include "blyt.h"

extern "C" void blyt_cart_on_new_state() {
    blyt_console_debug("new_state_called");
}

extern "C" void blyt_cart_init()   {}
extern "C" void blyt_cart_update() { blyt_quit(); }
extern "C" void blyt_cart_draw()   {}
"#,
        )
        .write(&project);

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());
    common::run_cart_native(&cart, "new_state_called");
}

/// blyt_cart_on_save_state and blyt_cart_on_load_state fire for C++ carts.
#[test]
fn cpp_cart_lifecycle_on_save_load_state() {
    require_sdk();
    require_cpp_sdk();

    let tmp = TempDir::new().unwrap();
    let save_dir = TempDir::new().unwrap();
    let project = tmp.path().join("cpp_lifecycle_sl");

    CartProject::new()
        .config(CART_CONFIG)
        .cpp(
            r#"
#include "blyt.h"
#include "cart_state.h"

extern "C" void blyt_cart_on_save_state() {
    blyt_console_debug("save_called");
}

extern "C" void blyt_cart_on_load_state(blyt_load_info_t info) {
    (void)info;
    blyt_console_debug("load_called");
}

extern "C" void blyt_cart_init() {
    int32_t slot = -1;
    blyt_buffer_alloc_slot(S_GAME, &slot);
    blyt_save_write(0);
}

extern "C" void blyt_cart_update() {
    blyt_save_read(0);
    blyt_quit();
}

extern "C" void blyt_cart_draw() {}
"#,
        )
        .write(&project);

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());

    let save_str = save_dir.path().to_str().unwrap();
    run_cart_native_with_env(&cart, &[("BLYT_SAVE_DIR", save_str)], "save_called");
    run_cart_native_with_env(&cart, &[("BLYT_SAVE_DIR", save_str)], "load_called");
}

// ── --reset-every-frame integration ───────────────────────────────────────

const GLOBALS_CONFIG: &str = "\
records:
  Globals:
    fields:
      - { name: frame, type: i32 }
state_buffers:
  globals:
    record: Globals
    count: 1
";

/// A C cart that properly stores its frame counter in a state buffer survives
/// the --reset-every-frame cycle: on_save_state → zero BSS → init → on_load_state.
#[test]
fn c_cart_reset_every_frame() {
    require_sdk();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("c_ref");

    CartProject::new()
        .config(GLOBALS_CONFIG)
        .c(r#"
#include "blyt.h"
#include "cart_state.h"
#include <stdio.h>

static int32_t s_frame = 0;

void blyt_cart_on_new_state(void) {}

void blyt_cart_on_save_state(void) {
    blyt_buffer_set_i32(S_GLOBALS, 0, S_GLOBALS_FRAME, s_frame);
}

void blyt_cart_on_load_state(blyt_load_info_t info) {
    (void)info;
    s_frame = blyt_buffer_get_i32(S_GLOBALS, 0, S_GLOBALS_FRAME);
}

void blyt_cart_init(void) {
    int32_t slot = -1;
    blyt_buffer_alloc_slot(S_GLOBALS, &slot);
}

void blyt_cart_update(void) {
    s_frame++;
    if (s_frame == 10) {
        char buf[32];
        snprintf(buf, sizeof(buf), "frame=%d", s_frame);
        blyt_console_debug(buf);
    }
}

void blyt_cart_draw(void) {}
"#)
        .write(&project);

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());
    run_cart_native_with_flags(
        &cart,
        &["--reset-every-frame", "--quit-after", "10"],
        "frame=10",
    );
}

/// A Lua cart that properly stores its frame counter in a state buffer survives
/// the --reset-every-frame cycle. Also covers the Lua lifecycle dispatch path
/// (blyt32lua.c) during each cycle's init/on_load_state calls.
#[test]
fn lua_cart_reset_every_frame() {
    require_sdk();
    require_lua_sdk();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("lua_ref");

    CartProject::new()
        .config(GLOBALS_CONFIG)
        .lua(
            r#"
local frame = 0

function on_new_state() end

function on_save_state()
    S.globals[0].frame = frame
end

function on_load_state(info)
    frame = S.globals[0].frame
end

function init()
    blyt.buf.alloc_slot(S.GLOBALS)
end

function update()
    frame = frame + 1
    if frame == 10 then
        blyt.debug.print("frame=" .. frame)
    end
end

function draw() end
"#,
        )
        .write(&project);

    let cart = build_lua_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());
    run_cart_native_with_flags(
        &cart,
        &["--reset-every-frame", "--quit-after", "10"],
        "frame=10",
    );
}

// ── WASM Lua lifecycle ─────────────────────────────────────────────────────

/// blyt_cart_on_new_state stub in blyt32lua_bridge.c (WASM path) is present and
/// does not crash. If the stub were missing the linker would error; if the bridge
/// tried to dispatch it as a real Lua call the WASM host would error.
#[test]
fn wasm_lua_cart_lifecycle_on_new_state() {
    require_sdk();
    require_lua_sdk();
    require_wasm();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("wasm_lua_lifecycle_new");

    CartProject::new()
        .lua(
            r#"
function on_new_state()
    blyt.debug.print("new_state_called")
end

function init()   end
function update() blyt.quit() end
function draw()   end
"#,
        )
        .write(&project);

    let cart = build_lua_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());
    run_cart_wasm(&cart, "new_state_called");
}

// ── WASM Lua state buffers ─────────────────────────────────────────────────

/// blyt.buf.* and blyt.save_write/read work for a pure Lua WASM cart.
/// Exercises the host-side wasm_register_state_api path (not the ECALL stubs
/// used by native Lua carts).
#[test]
fn wasm_lua_cart_state_buffer_round_trips() {
    require_sdk();
    require_lua_sdk();
    require_wasm();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("wasm_lua_sb_rt");

    CartProject::new()
        .config(CART_CONFIG)
        .lua(
            r#"
-- Use raw buffer/field IDs (buf_id=1, field_idx=1) to avoid the S proxy
-- which is native C code not available in the WASM Lua VM.
local slot = -1

function init()
    slot = blyt.buf.alloc_slot(1)
    blyt.buf.set_i32(1, slot, 1, 42)
    blyt.save_write(0)
    blyt.buf.set_i32(1, slot, 1, 99)
end

function update()
    blyt.save_read(0)
    local score = blyt.buf.get_i32(1, 0, 1)
    blyt.debug.print("score=" .. tostring(score))
    blyt.quit()
end

function draw() end
"#,
        )
        .write(&project);

    let cart = build_lua_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());
    run_cart_wasm_with_env(&cart, &[("BLYT_SAVE_DIR", "/tmp")], "score=42");
}

// ── WASM reset-every-frame ─────────────────────────────────────────────────

/// A C cart that stores its frame counter in a state buffer survives the
/// WASM reset-every-frame cycle (wasm_loop calls blyt_reset_every_frame_cycle
/// after each BLYT_RUN_FRAME_DONE).
#[test]
fn wasm_c_cart_reset_every_frame() {
    require_sdk();
    require_wasm();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("wasm_c_ref");

    CartProject::new()
        .config(GLOBALS_CONFIG)
        .c(r#"
#include "blyt.h"
#include "cart_state.h"
#include <stdio.h>

static int32_t s_frame = 0;

void blyt_cart_on_new_state(void) {}

void blyt_cart_on_save_state(void) {
    blyt_buffer_set_i32(S_GLOBALS, 0, S_GLOBALS_FRAME, s_frame);
}

void blyt_cart_on_load_state(blyt_load_info_t info) {
    (void)info;
    s_frame = blyt_buffer_get_i32(S_GLOBALS, 0, S_GLOBALS_FRAME);
}

void blyt_cart_init(void) {
    int32_t slot = -1;
    blyt_buffer_alloc_slot(S_GLOBALS, &slot);
}

void blyt_cart_update(void) {
    s_frame++;
    if (s_frame == 3) {
        char buf[32];
        snprintf(buf, sizeof(buf), "frame=%d", s_frame);
        blyt_console_debug(buf);
        blyt_quit();
    }
}

void blyt_cart_draw(void) {}
"#)
        .write(&project);

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());
    run_cart_wasm_with_env(&cart, &[("BLYT_RESET_EVERY_FRAME", "1")], "frame=3");
}

/// A Lua cart that stores its frame counter in a state buffer survives the
/// WASM reset-every-frame cycle (wasm_lua_reset_cycle: full VM teardown +
/// recreate, on_save_state/on_load_state, state snapshot/restore).
/// init() intentionally contains no logging — it runs on every reset cycle.
#[test]
fn wasm_lua_cart_reset_every_frame() {
    require_sdk();
    require_lua_sdk();
    require_wasm();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("wasm_lua_ref");

    CartProject::new()
        .config(GLOBALS_CONFIG)
        .lua(
            r#"
-- Use raw buffer/field IDs (buf_id=1, field_idx=1) to avoid the S proxy
-- which is native C code not available in the WASM Lua VM.
local frame = 0

function on_save_state()
    blyt.buf.set_i32(1, 0, 1, frame)
end

function on_load_state(info)
    frame = blyt.buf.get_i32(1, 0, 1)
end

function init()
    blyt.buf.alloc_slot(1)
end

function update()
    frame = frame + 1
    if frame == 3 then
        blyt.debug.print("frame=" .. tostring(frame))
        blyt.quit()
    end
end

function draw() end
"#,
        )
        .write(&project);

    let cart = build_lua_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());
    run_cart_wasm_with_env(&cart, &[("BLYT_RESET_EVERY_FRAME", "1")], "frame=3");
}
