mod common;

use common::{
    CartProject, build_cart, build_lua_cart, require_cpp_sdk, require_libretro_core,
    require_lua_sdk, require_rust_riscv_target, require_sdk, require_wasm,
    run_cart_all_legs_exact_reset_every_frame, run_cart_all_legs_exact_with_save_dir,
    run_cart_cross_version_all_legs, run_cart_libretro, run_cart_libretro_with_flags,
    run_cart_native_with_env, run_cart_native_with_flags, run_cart_wasm, run_cart_wasm_with_env,
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

/// Same schema as CART_CONFIG, but declares a non-zero save_version so the
/// runtime stamps it into the save header and reports it back on load (Gap C).
const SAVE_VERSION_7_CONFIG: &str = "\
save_version: 7
records:
  Game:
    fields:
      - { name: score, type: i32 }
state_buffers:
  game:
    record: Game
    count: 1
";

/// Cross-version round trip (issue #112): a writer cart and a reader cart that
/// share an identical state-buffer schema but declare different save_versions.
/// The reader must observe the *writer's* version on load.
const SAVE_VERSION_1_CONFIG: &str = "\
save_version: 1
records:
  Game:
    fields:
      - { name: score, type: i32 }
state_buffers:
  game:
    record: Game
    count: 1
";
const SAVE_VERSION_2_CONFIG: &str = "\
save_version: 2
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
      - { name: v_f64,  type: f64  }
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

/// All field types (incl. f64, Spike U) survive a save/load round-trip for a Rust cart.
#[test]
fn rust_cart_all_field_types_round_trips() {
    require_sdk();
    require_rust_riscv_target();

    let tmp = TempDir::new().unwrap();
    let save_dir = TempDir::new().unwrap();
    let project = tmp.path().join("rust_sb_types");

    CartProject::new()
        .config(ALL_TYPES_CONFIG)
        .rust(
            r#"#![no_std]
include!(env!("BLYT_CART_STATE_RS"));

use blyt::buffer::{
    alloc_slot, get_bool, get_f32, get_f64, get_i8, get_i16, get_u8, get_u16, get_u32,
    set_bool, set_f32, set_f64, set_i8, set_i16, set_u8, set_u16, set_u32,
};
use blyt::save::{save_read, save_write};

#[no_mangle]
pub extern "C" fn blyt_cart_init() {
    let slot = alloc_slot(S_TYPES);
    set_f32(S_TYPES, slot, S_TYPES_V_F32, 1.5_f32);
    set_f64(S_TYPES, slot, S_TYPES_V_F64, 0.123456789012345_f64);
    set_u32(S_TYPES, slot, S_TYPES_V_U32, 0xDEAD_BEEF_u32);
    set_i8(S_TYPES, slot, S_TYPES_V_I8, -42_i8);
    set_u8(S_TYPES, slot, S_TYPES_V_U8, 200_u8);
    set_i16(S_TYPES, slot, S_TYPES_V_I16, -1000_i16);
    set_u16(S_TYPES, slot, S_TYPES_V_U16, 60000_u16);
    set_bool(S_TYPES, slot, S_TYPES_V_BOOL, true);
    save_write(0);
    set_f32(S_TYPES, slot, S_TYPES_V_F32, 0.0_f32);
    set_f64(S_TYPES, slot, S_TYPES_V_F64, 0.0_f64);
    set_u32(S_TYPES, slot, S_TYPES_V_U32, 0);
    set_i8(S_TYPES, slot, S_TYPES_V_I8, 0);
    set_u8(S_TYPES, slot, S_TYPES_V_U8, 0);
    set_i16(S_TYPES, slot, S_TYPES_V_I16, 0);
    set_u16(S_TYPES, slot, S_TYPES_V_U16, 0);
    set_bool(S_TYPES, slot, S_TYPES_V_BOOL, false);
}

#[no_mangle]
pub extern "C" fn blyt_cart_update() {
    save_read(0);
    let vf = get_f32(S_TYPES, 0, S_TYPES_V_F32);
    let vd = get_f64(S_TYPES, 0, S_TYPES_V_F64);
    let vu = get_u32(S_TYPES, 0, S_TYPES_V_U32);
    let vi8 = get_i8(S_TYPES, 0, S_TYPES_V_I8);
    let vu8 = get_u8(S_TYPES, 0, S_TYPES_V_U8);
    let vi16 = get_i16(S_TYPES, 0, S_TYPES_V_I16);
    let vu16 = get_u16(S_TYPES, 0, S_TYPES_V_U16);
    let vb = get_bool(S_TYPES, 0, S_TYPES_V_BOOL);
    if vf == 1.5_f32
        && vd == 0.123456789012345_f64
        && vu == 0xDEAD_BEEF_u32
        && vi8 == -42_i8
        && vu8 == 200_u8
        && vi16 == -1000_i16
        && vu16 == 60000_u16
        && vb
    {
        blyt::console_debug("types_ok");
    } else {
        blyt::console_debug("types_fail");
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
        "types_ok",
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

/// f64 field survives a save/load round-trip for a native Lua cart (Spike U).
#[test]
fn lua_cart_all_field_types_round_trips() {
    require_sdk();
    require_lua_sdk();

    let tmp = TempDir::new().unwrap();
    let save_dir = TempDir::new().unwrap();
    let project = tmp.path().join("lua_sb_types");

    CartProject::new()
        .config(ALL_TYPES_CONFIG)
        .lua(
            r#"
-- buf_id=1 (types), field_idx=2 (v_f64).
local slot = -1
local EXPECT = 0.123456789012345

function init()
    slot = blyt.buf.alloc_slot(1)
    blyt.buf.set_f64(1, slot, 2, EXPECT)
    blyt.save_write(0)
    blyt.buf.set_f64(1, slot, 2, 0.0)
end

function update()
    blyt.save_read(0)
    local v = blyt.buf.get_f64(1, 0, 2)
    if v == EXPECT then
        blyt.debug.print("types_ok")
    else
        blyt.debug.print("types_fail")
    end
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
        "types_ok",
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

/// All field types (incl. f64, Spike U) survive a save/load round-trip.
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
    blyt_buffer_set_f64 (S_TYPES, slot, S_TYPES_V_F64,  0.123456789012345);
    blyt_buffer_set_u32 (S_TYPES, slot, S_TYPES_V_U32,  0xDEADBEEFu);
    blyt_buffer_set_i8  (S_TYPES, slot, S_TYPES_V_I8,   -42);
    blyt_buffer_set_u8  (S_TYPES, slot, S_TYPES_V_U8,   200);
    blyt_buffer_set_i16 (S_TYPES, slot, S_TYPES_V_I16,  -1000);
    blyt_buffer_set_u16 (S_TYPES, slot, S_TYPES_V_U16,  60000);
    blyt_buffer_set_bool(S_TYPES, slot, S_TYPES_V_BOOL, 1);
    blyt_save_write(0);
    blyt_buffer_set_f32 (S_TYPES, slot, S_TYPES_V_F32,  0.0f);
    blyt_buffer_set_f64 (S_TYPES, slot, S_TYPES_V_F64,  0.0);
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
    double   vd  = blyt_buffer_get_f64 (S_TYPES, 0, S_TYPES_V_F64);
    uint32_t vu  = blyt_buffer_get_u32 (S_TYPES, 0, S_TYPES_V_U32);
    int8_t   vi8 = blyt_buffer_get_i8  (S_TYPES, 0, S_TYPES_V_I8);
    uint8_t  vu8 = blyt_buffer_get_u8  (S_TYPES, 0, S_TYPES_V_U8);
    int16_t  vi16 = blyt_buffer_get_i16(S_TYPES, 0, S_TYPES_V_I16);
    uint16_t vu16 = blyt_buffer_get_u16(S_TYPES, 0, S_TYPES_V_U16);
    int      vb  = blyt_buffer_get_bool(S_TYPES, 0, S_TYPES_V_BOOL) ? 1 : 0;
    if (vf == 1.5f && vd == 0.123456789012345 && vu == 0xDEADBEEFu && vi8 == -42 && vu8 == 200
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

/// Hybrid (Lua + C) cart accessing state buffers via the Lua `S` proxy on WASM.
/// Regression for the `S` proxy not being registered for hybrid carts in the
/// host-Lua fast path (wasm_register_s_proxy read g_lua_state_ctx directly,
/// which is NULL for hybrid carts), so any S.* access errored and aborted the
/// WASM main loop. The native leg always worked; only WASM was affected.
#[test]
fn wasm_hybrid_lua_state_buffer_round_trips() {
    require_sdk();
    require_wasm();
    require_lua_sdk();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("wsb_hybrid");

    CartProject::new()
        .config(CART_CONFIG)
        // A C export makes this a hybrid cart (has .lua_exports), so it takes
        // the g_session path where g_lua_state_ctx is NULL.
        .c(r#"
#include "blyt.h"

BLYT_LUA_EXPORT_VOID(work_native) {
    /* no-op: presence of a Lua export is what makes this a hybrid cart */
}
"#)
        .lua(
            r#"
local slot = -1

function init()
    slot = blyt.buf.alloc_slot(S.GAME)
    S.game[slot].score = 42
end

function update()
    work_native()
    blyt.debug.print("hybrid_score=" .. tostring(S.game[slot].score))
    blyt.quit()
end

function draw() end
"#,
        )
        .write(&project);

    let cart = build_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());
    run_cart_wasm(&cart, "hybrid_score=42");
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
    blyt_buffer_set_f64 (S_TYPES, slot, S_TYPES_V_F64,  0.123456789012345);
    blyt_buffer_set_u32 (S_TYPES, slot, S_TYPES_V_U32,  0xDEADBEEFu);
    blyt_buffer_set_i8  (S_TYPES, slot, S_TYPES_V_I8,   -42);
    blyt_buffer_set_u8  (S_TYPES, slot, S_TYPES_V_U8,   200);
    blyt_buffer_set_i16 (S_TYPES, slot, S_TYPES_V_I16,  -1000);
    blyt_buffer_set_u16 (S_TYPES, slot, S_TYPES_V_U16,  60000);
    blyt_buffer_set_bool(S_TYPES, slot, S_TYPES_V_BOOL, 1);
    blyt_save_write(0);
    blyt_buffer_set_f32 (S_TYPES, slot, S_TYPES_V_F32,  0.0f);
    blyt_buffer_set_f64 (S_TYPES, slot, S_TYPES_V_F64,  0.0);
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
    double   vd   = blyt_buffer_get_f64 (S_TYPES, 0, S_TYPES_V_F64);
    uint32_t vu   = blyt_buffer_get_u32 (S_TYPES, 0, S_TYPES_V_U32);
    int8_t   vi8  = blyt_buffer_get_i8  (S_TYPES, 0, S_TYPES_V_I8);
    uint8_t  vu8  = blyt_buffer_get_u8  (S_TYPES, 0, S_TYPES_V_U8);
    int16_t  vi16 = blyt_buffer_get_i16 (S_TYPES, 0, S_TYPES_V_I16);
    uint16_t vu16 = blyt_buffer_get_u16 (S_TYPES, 0, S_TYPES_V_U16);
    int      vb   = blyt_buffer_get_bool(S_TYPES, 0, S_TYPES_V_BOOL) ? 1 : 0;
    if (vf == 1.5f && vd == 0.123456789012345 && vu == 0xDEADBEEFu && vi8 == -42 && vu8 == 200
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

/// All field types (incl. f64, Spike U) survive a save/load round-trip for a
/// Rust cart on the WASM target.
#[test]
fn wasm_rust_cart_all_field_types_round_trips() {
    require_sdk();
    require_rust_riscv_target();
    require_wasm();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("wasm_rust_sb_types");

    CartProject::new()
        .config(ALL_TYPES_CONFIG)
        .rust(
            r#"#![no_std]
include!(env!("BLYT_CART_STATE_RS"));

use blyt::buffer::{
    alloc_slot, get_bool, get_f32, get_f64, get_i8, get_i16, get_u8, get_u16, get_u32,
    set_bool, set_f32, set_f64, set_i8, set_i16, set_u8, set_u16, set_u32,
};
use blyt::save::{save_read, save_write};

#[no_mangle]
pub extern "C" fn blyt_cart_init() {
    let slot = alloc_slot(S_TYPES);
    set_f32(S_TYPES, slot, S_TYPES_V_F32, 1.5_f32);
    set_f64(S_TYPES, slot, S_TYPES_V_F64, 0.123456789012345_f64);
    set_u32(S_TYPES, slot, S_TYPES_V_U32, 0xDEAD_BEEF_u32);
    set_i8(S_TYPES, slot, S_TYPES_V_I8, -42_i8);
    set_u8(S_TYPES, slot, S_TYPES_V_U8, 200_u8);
    set_i16(S_TYPES, slot, S_TYPES_V_I16, -1000_i16);
    set_u16(S_TYPES, slot, S_TYPES_V_U16, 60000_u16);
    set_bool(S_TYPES, slot, S_TYPES_V_BOOL, true);
    save_write(0);
    set_f32(S_TYPES, slot, S_TYPES_V_F32, 0.0_f32);
    set_f64(S_TYPES, slot, S_TYPES_V_F64, 0.0_f64);
    set_u32(S_TYPES, slot, S_TYPES_V_U32, 0);
    set_i8(S_TYPES, slot, S_TYPES_V_I8, 0);
    set_u8(S_TYPES, slot, S_TYPES_V_U8, 0);
    set_i16(S_TYPES, slot, S_TYPES_V_I16, 0);
    set_u16(S_TYPES, slot, S_TYPES_V_U16, 0);
    set_bool(S_TYPES, slot, S_TYPES_V_BOOL, false);
}

#[no_mangle]
pub extern "C" fn blyt_cart_update() {
    save_read(0);
    let vf = get_f32(S_TYPES, 0, S_TYPES_V_F32);
    let vd = get_f64(S_TYPES, 0, S_TYPES_V_F64);
    let vu = get_u32(S_TYPES, 0, S_TYPES_V_U32);
    let vi8 = get_i8(S_TYPES, 0, S_TYPES_V_I8);
    let vu8 = get_u8(S_TYPES, 0, S_TYPES_V_U8);
    let vi16 = get_i16(S_TYPES, 0, S_TYPES_V_I16);
    let vu16 = get_u16(S_TYPES, 0, S_TYPES_V_U16);
    let vb = get_bool(S_TYPES, 0, S_TYPES_V_BOOL);
    if vf == 1.5_f32
        && vd == 0.123456789012345_f64
        && vu == 0xDEAD_BEEF_u32
        && vi8 == -42_i8
        && vu8 == 200_u8
        && vi16 == -1000_i16
        && vu16 == 60000_u16
        && vb
    {
        blyt::console_debug("types_ok");
    } else {
        blyt::console_debug("types_fail");
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

/// Cart for the #98 regression: the Lua lifecycle glue must marshal
/// blyt_load_info_t into a Lua `info` table so `on_load_state(info)` can read
/// `info.reason` — previously native saw `info == nil`. The --reset-every-frame
/// cycle restores state with reason BLYT_LOAD_HOT_RELOAD (3); the cart records
/// the reason it observed and prints it at frame 10, then self-quits.
const LUA_LOAD_INFO_CART: &str = r#"
local frame = 0
local last_reason = -1

function on_new_state() end

function on_save_state()
    S.globals[0].frame = frame
end

function on_load_state(info)
    frame = S.globals[0].frame
    last_reason = info.reason
end

function init()
    blyt.buf.alloc_slot(S.GLOBALS)
end

function update()
    frame = frame + 1
    if frame == 10 then
        blyt.debug.print("<sb:reason=" .. last_reason .. ">")
        blyt.quit()
    end
end

function draw() end
"#;

/// Regression for #98, across all three legs. The --reset-every-frame cycle
/// restores with BLYT_LOAD_HOT_RELOAD (3) on native, WASM and libretro alike, so
/// the same cart must observe `info.reason == 3` identically on every leg — the
/// native/WASM divergence #98 reported (native saw `info == nil`) is gone. Driven
/// through `run_cart_all_legs_exact_reset_every_frame` so any future per-leg
/// divergence — in the value or the number of times it is emitted — fails by
/// construction.
#[test]
fn lua_cart_on_load_state_receives_reason() {
    require_sdk();
    require_lua_sdk();
    require_wasm();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("lua_load_info");

    CartProject::new()
        .config(GLOBALS_CONFIG)
        .lua(LUA_LOAD_INFO_CART)
        .write(&project);

    let cart = build_lua_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());
    // Exact marker sequence (#284): the cart records the reason exactly once, so
    // a leg that runs an extra reset cycle or repeats the line fails here rather
    // than passing a substring match.
    run_cart_all_legs_exact_reset_every_frame(&cart, "sb", &["reason=3"]);
}

/// Cart for Gap A of issue #110: the most common real load path — a cart that
/// saves and loads *itself* via `blyt.save_read` — had zero coverage of the
/// `info` it receives. In `update()` the cart writes score=42, saves, clobbers
/// to 99, then reads slot 0 back. `blyt_save_read` fires `on_load_state` with
/// `info{reason=SAVE_GAME(0), saved_cart_version=<the cart's declared
/// save_version>}` (Gap C, issue #112: the version is read from the save
/// header the cart itself just wrote). The cart prints what it observed AND the
/// restored score, so the assertion proves the value was delivered and the
/// buffer actually round-tripped — not merely that the callback fired.
const LUA_SAVE_READ_INFO_CART: &str = r#"
local slot = -1
local got_reason = -1
local got_version = -1

function on_load_state(info)
    got_reason = info.reason
    got_version = info.saved_cart_version
end

function init()
    slot = blyt.buf.alloc_slot(S.GAME)
end

function update()
    S.game[slot].score = 42
    blyt.save_write(0)
    S.game[slot].score = 99
    blyt.save_read(0)
    blyt.debug.print(
        "<m:save_read reason="
            .. got_reason
            .. " version="
            .. got_version
            .. " score="
            .. S.game[slot].score
            .. ">"
    )
    blyt.quit()
end

function draw() end
"#;

/// Gap A (issue #110) + Gap C (issue #112): cart-initiated `blyt.save_read`
/// delivers `on_load_state` the correct `info` on every leg. The reason is
/// SAVE_GAME (0) and `saved_cart_version` is the cart's declared `save_version`
/// (7 here, read back from the save header the cart just wrote), identical
/// across native / WASM / libretro, with the buffer restored to 42.
#[test]
fn lua_cart_save_read_delivers_load_info() {
    require_sdk();
    require_lua_sdk();
    require_wasm();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("lua_save_read_info");

    CartProject::new()
        .config(SAVE_VERSION_7_CONFIG)
        .lua(LUA_SAVE_READ_INFO_CART)
        .write(&project);

    let cart = build_lua_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());
    run_cart_all_legs_exact_with_save_dir(&cart, "m", &["save_read reason=0 version=7 score=42"]);
}

/// Writer for the cross-version round trip: declares save_version=1, writes
/// score=42 to slot 0, then quits.
const LUA_CROSS_VERSION_WRITER: &str = r#"
function init() end

function update()
    local slot = blyt.buf.alloc_slot(S.GAME)
    S.game[slot].score = 42
    blyt.save_write(0)
    blyt.debug.print("writer save_version=1 wrote score=42")
    blyt.quit()
end

function draw() end
"#;

/// Reader for the cross-version round trip: declares save_version=2, loads the
/// save the writer produced, and reports the version it observed. It must see
/// the *writer's* version (1), proving the value comes from the save header,
/// not the reader's own manifest (2).
const LUA_CROSS_VERSION_READER: &str = r#"
local got_version = -1

function on_load_state(info)
    got_version = info.saved_cart_version
end

function update()
    blyt.save_read(0)
    blyt.debug.print(
        "reader save_version=2 saw version=" .. got_version .. " score=" .. S.game[0].score
    )
    blyt.quit()
end

function init() end

function draw() end
"#;

/// Gap C (issue #112): a v1 cart writes a save; a v2 cart reads it and observes
/// `saved_cart_version == 1` (the version that *wrote* the save), identical
/// across native / WASM / libretro. This is the load-info value's reason for
/// existing — it lets a cart migrate saves written by an older version.
#[test]
fn lua_cross_version_save_read_reports_writer_version() {
    require_sdk();
    require_lua_sdk();
    require_wasm();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    // Same leaf name → same cart id "xver" (the save path is keyed by id), in
    // separate parent dirs so the two projects do not collide on disk.
    let writer_dir = tmp.path().join("w").join("xver");
    let reader_dir = tmp.path().join("r").join("xver");

    CartProject::new()
        .config(SAVE_VERSION_1_CONFIG)
        .lua(LUA_CROSS_VERSION_WRITER)
        .write(&writer_dir);
    CartProject::new()
        .config(SAVE_VERSION_2_CONFIG)
        .lua(LUA_CROSS_VERSION_READER)
        .write(&reader_dir);

    let writer = build_lua_cart(&writer_dir);
    let reader = build_lua_cart(&reader_dir);
    assert!(writer.exists() && reader.exists());

    run_cart_cross_version_all_legs(
        &writer,
        &reader,
        "xver",
        "writer save_version=1 wrote score=42",
        "reader save_version=2 saw version=1 score=42",
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

/// f64 field survives a save/load round-trip for a pure-Lua WASM cart (Spike U).
/// Exercises the host-side wasm_register_state_api path (wasm_buf_get/set_f64).
#[test]
fn wasm_lua_cart_all_field_types_round_trips() {
    require_sdk();
    require_lua_sdk();
    require_wasm();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("wasm_lua_sb_types");

    CartProject::new()
        .config(ALL_TYPES_CONFIG)
        .lua(
            r#"
-- buf_id=1 (types), field_idx=2 (v_f64).
local slot = -1
local EXPECT = 0.123456789012345

function init()
    slot = blyt.buf.alloc_slot(1)
    blyt.buf.set_f64(1, slot, 2, EXPECT)
    blyt.save_write(0)
    blyt.buf.set_f64(1, slot, 2, 0.0)
end

function update()
    blyt.save_read(0)
    local v = blyt.buf.get_f64(1, 0, 2)
    if v == EXPECT then
        blyt.debug.print("types_ok")
    else
        blyt.debug.print("types_fail")
    end
    blyt.quit()
end

function draw() end
"#,
        )
        .write(&project);

    let cart = build_lua_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());
    run_cart_wasm_with_env(&cart, &[("BLYT_SAVE_DIR", "/tmp")], "types_ok");
}

/// S proxy global works in the WASM pure-Lua path.  wasm_register_s_proxy()
/// generates the S table with integer constants and proxy metatables so carts
/// can use S.GAME, S.game[slot].score, etc. without the rv32emu.
#[test]
fn wasm_lua_cart_s_proxy() {
    require_sdk();
    require_lua_sdk();
    require_wasm();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("wasm_lua_s_proxy");

    CartProject::new()
        .config(CART_CONFIG)
        .lua(
            r#"
local slot = -1

function init()
    slot = blyt.buf.alloc_slot(S.GAME)
    S.game[slot].score = 77
end

function update()
    local v = S.game[slot].score
    blyt.debug.print("score=" .. tostring(v))
    blyt.quit()
end

function draw() end
"#,
        )
        .write(&project);

    let cart = build_lua_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());
    run_cart_wasm(&cart, "score=77");
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

// ── Embedded libretro core legs ─────────────────────────────────────────────
//
// Third leg of the native/wasm pairs above: the same functionality through
// test_libretro_core, which dlopens blyt_libretro.so with its EMBEDDED guest
// lib blobs.  The reset-every-frame pairs map onto the core's own snapshot
// mechanism: --reset-every-frame retro_serialize + retro_unserialize
// after every frame (the rewind/netplay path), driving the same
// on_save_state / on_load_state cart hooks.

/// blyt_cart_on_new_state lifecycle hook fires through the embedded core.
#[test]
fn libretro_lua_cart_lifecycle_on_new_state() {
    require_sdk();
    require_lua_sdk();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("libretro_lua_lifecycle_new");

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
    run_cart_libretro(&cart, "new_state_called");
}

/// S proxy (S.game[slot].score) works through the embedded core — exercises
/// the packer-generated native S glue inside the embedded libblyt32lua.so.
#[test]
fn libretro_lua_cart_s_proxy() {
    require_sdk();
    require_lua_sdk();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("libretro_lua_s_proxy");

    CartProject::new()
        .config(CART_CONFIG)
        .lua(
            r#"
local slot = -1

function init()
    slot = blyt.buf.alloc_slot(S.GAME)
    S.game[slot].score = 77
end

function update()
    local v = S.game[slot].score
    blyt.debug.print("score=" .. tostring(v))
    blyt.quit()
end

function draw() end
"#,
        )
        .write(&project);

    let cart = build_lua_cart(&project);
    assert!(cart.exists(), "cart not found at {}", cart.display());
    run_cart_libretro(&cart, "score=77");
}

/// A C cart that stores its frame counter in a state buffer survives the
/// reset-every-frame cycle driven through the embedded core.
#[test]
fn libretro_c_cart_reset_every_frame() {
    require_sdk();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("libretro_c_rt");

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
    run_cart_libretro_with_flags(&cart, &["--reset-every-frame"], "frame=3");
}

/// A Lua cart that stores its frame counter in a state buffer survives the
/// reset-every-frame cycle driven through the embedded core.
#[test]
fn libretro_lua_cart_reset_every_frame() {
    require_sdk();
    require_lua_sdk();
    require_libretro_core();

    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("libretro_lua_rt");

    CartProject::new()
        .config(GLOBALS_CONFIG)
        .lua(
            r#"
local frame = 0

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
    run_cart_libretro_with_flags(&cart, &["--reset-every-frame"], "frame=3");
}

// ── Packed entity refs (ADR-0096): generation counters + ref/ref_valid ─────

/// Two buffers; Globals carries a `ref:` field into entity (also exercises
/// the devtool's ref: schema support end to end on every leg).
const ENTITY_REF_CONFIG: &str = "\
records:
  Globals:
    fields:
      - { name: frame,  type: i32 }
      - { name: target, ref: entity }
  Entity:
    fields:
      - { name: hp, type: i32 }
state_buffers:
  globals:
    record: Globals
    count: 1
  entity:
    record: Entity
    count: 4
";

/// alloc → ref packs (gen=1)<<16|slot; valid; ref_slot round-trips; free →
/// stale; realloc of the same slot → old ref still stale (gen=2), new valid.
const C_REF_LIFECYCLE: &str = r#"
#include "blyt.h"
#include "cart_state.h"

void blyt_cart_init(void) {}

void blyt_cart_update(void) {
    int ok = 1;
    int32_t g = -1, a = -1, b = -1;
    blyt_buffer_alloc_slot(S_GLOBALS, &g);
    blyt_buffer_alloc_slot(S_ENTITY, &a);
    blyt_entity_ref_t r = blyt_buffer_ref(S_ENTITY, a);
    ok &= (r == ((1u << 16) | (uint32_t)a));
    ok &= blyt_buffer_ref_valid(S_ENTITY, r);
    ok &= (blyt_buffer_ref_slot(r) == a);
    blyt_buffer_free_slot(S_ENTITY, a);
    ok &= !blyt_buffer_ref_valid(S_ENTITY, r);
    blyt_buffer_alloc_slot(S_ENTITY, &b);
    ok &= (b == a);                           /* same slot reused */
    ok &= !blyt_buffer_ref_valid(S_ENTITY, r); /* old ref stays stale */
    blyt_entity_ref_t r2 = blyt_buffer_ref(S_ENTITY, b);
    ok &= (r2 == ((2u << 16) | (uint32_t)b));
    ok &= blyt_buffer_ref_valid(S_ENTITY, r2);
    blyt_console_debug(ok ? "ref_ok" : "ref_bad");
    blyt_quit();
}

void blyt_cart_draw(void) {}
"#;

const RUST_REF_LIFECYCLE: &str = r#"#![no_std]
include!(env!("BLYT_CART_STATE_RS"));

use blyt::buffer::{alloc_slot, entity_ref, free_slot, ref_slot, ref_valid};

#[no_mangle]
pub extern "C" fn blyt_cart_init() {}

#[no_mangle]
pub extern "C" fn blyt_cart_update() {
    let mut ok = true;
    let _g = alloc_slot(S_GLOBALS);
    let a = alloc_slot(S_ENTITY);
    let r = entity_ref(S_ENTITY, a);
    ok &= r == (1u32 << 16) | a as u32;
    ok &= ref_valid(S_ENTITY, r);
    ok &= ref_slot(r) == a;
    free_slot(S_ENTITY, a);
    ok &= !ref_valid(S_ENTITY, r);
    let b = alloc_slot(S_ENTITY);
    ok &= b == a;
    ok &= !ref_valid(S_ENTITY, r);
    let r2 = entity_ref(S_ENTITY, b);
    ok &= r2 == (2u32 << 16) | b as u32;
    ok &= ref_valid(S_ENTITY, r2);
    blyt::console_debug(if ok { "ref_ok" } else { "ref_bad" });
    blyt::quit();
}

#[no_mangle]
pub extern "C" fn blyt_cart_draw() {}
"#;

const LUA_REF_LIFECYCLE: &str = r#"
function init() end

function update()
    blyt.buf.alloc_slot(S.GLOBALS)
    local a = blyt.buf.alloc_slot(S.ENTITY)
    local r = blyt.buf.ref(S.ENTITY, a)
    local ok = (r == ((1 << 16) | a))
    ok = ok and blyt.buf.ref_valid(S.ENTITY, r)
    ok = ok and (blyt.buf.ref_slot(r) == a)
    blyt.buf.free_slot(S.ENTITY, a)
    ok = ok and not blyt.buf.ref_valid(S.ENTITY, r)
    local b = blyt.buf.alloc_slot(S.ENTITY)
    ok = ok and (b == a)
    ok = ok and not blyt.buf.ref_valid(S.ENTITY, r)
    local r2 = blyt.buf.ref(S.ENTITY, b)
    ok = ok and (r2 == ((2 << 16) | b))
    ok = ok and blyt.buf.ref_valid(S.ENTITY, r2)
    blyt.debug.print(ok and "ref_ok" or "ref_bad")
    blyt.quit()
end

function draw() end
"#;

#[test]
fn c_cart_entity_ref_lifecycle() {
    require_sdk();
    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("ref_c");
    CartProject::new()
        .config(ENTITY_REF_CONFIG)
        .c(C_REF_LIFECYCLE)
        .write(&project);
    let cart = build_cart(&project);
    run_cart_native_with_flags(&cart, &[], "ref_ok");
}

#[test]
fn wasm_c_cart_entity_ref_lifecycle() {
    require_sdk();
    require_wasm();
    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("ref_c_wasm");
    CartProject::new()
        .config(ENTITY_REF_CONFIG)
        .c(C_REF_LIFECYCLE)
        .write(&project);
    let cart = build_cart(&project);
    run_cart_wasm(&cart, "ref_ok");
}

#[test]
fn libretro_c_cart_entity_ref_lifecycle() {
    require_sdk();
    require_libretro_core();
    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("ref_c_lr");
    CartProject::new()
        .config(ENTITY_REF_CONFIG)
        .c(C_REF_LIFECYCLE)
        .write(&project);
    let cart = build_cart(&project);
    run_cart_libretro(&cart, "ref_ok");
}

#[test]
fn rust_cart_entity_ref_lifecycle() {
    require_sdk();
    require_rust_riscv_target();
    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("ref_rust");
    CartProject::new()
        .config(ENTITY_REF_CONFIG)
        .rust(RUST_REF_LIFECYCLE)
        .write(&project);
    let cart = build_cart(&project);
    run_cart_native_with_flags(&cart, &[], "ref_ok");
}

#[test]
fn wasm_rust_cart_entity_ref_lifecycle() {
    require_sdk();
    require_rust_riscv_target();
    require_wasm();
    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("ref_rust_wasm");
    CartProject::new()
        .config(ENTITY_REF_CONFIG)
        .rust(RUST_REF_LIFECYCLE)
        .write(&project);
    let cart = build_cart(&project);
    run_cart_wasm(&cart, "ref_ok");
}

#[test]
fn libretro_rust_cart_entity_ref_lifecycle() {
    require_sdk();
    require_rust_riscv_target();
    require_libretro_core();
    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("ref_rust_lr");
    CartProject::new()
        .config(ENTITY_REF_CONFIG)
        .rust(RUST_REF_LIFECYCLE)
        .write(&project);
    let cart = build_cart(&project);
    run_cart_libretro(&cart, "ref_ok");
}

#[test]
fn lua_cart_entity_ref_lifecycle() {
    require_sdk();
    require_lua_sdk();
    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("ref_lua");
    CartProject::new()
        .config(ENTITY_REF_CONFIG)
        .lua(LUA_REF_LIFECYCLE)
        .write(&project);
    let cart = build_lua_cart(&project);
    run_cart_native_with_flags(&cart, &[], "ref_ok");
}

/// The WASM leg of the Lua lifecycle specifically covers the host-Lua
/// fast-path closures in wasm_main.c (distinct code from blyt32lua.c).
#[test]
fn wasm_lua_cart_entity_ref_lifecycle() {
    require_sdk();
    require_lua_sdk();
    require_wasm();
    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("ref_lua_wasm");
    CartProject::new()
        .config(ENTITY_REF_CONFIG)
        .lua(LUA_REF_LIFECYCLE)
        .write(&project);
    let cart = build_lua_cart(&project);
    run_cart_wasm(&cart, "ref_ok");
}

#[test]
fn libretro_lua_cart_entity_ref_lifecycle() {
    require_sdk();
    require_lua_sdk();
    require_libretro_core();
    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("ref_lua_lr");
    CartProject::new()
        .config(ENTITY_REF_CONFIG)
        .lua(LUA_REF_LIFECYCLE)
        .write(&project);
    let cart = build_lua_cart(&project);
    run_cart_libretro(&cart, "ref_ok");
}

/// Edge cases: refs to unallocated/out-of-range slots and bad buffer handles
/// are NONE; NONE is never valid; a ref is only valid against a buffer where
/// its slot is actually allocated; slot 0's first ref is exactly 0x00010000.
const C_REF_EDGE: &str = r#"
#include "blyt.h"
#include "cart_state.h"

void blyt_cart_init(void) {}

void blyt_cart_update(void) {
    int ok = 1;
    /* nothing allocated yet */
    ok &= (blyt_buffer_ref(S_ENTITY, 0) == BLYT_ENTITY_REF_NONE);
    ok &= (blyt_buffer_ref(S_ENTITY, 99) == BLYT_ENTITY_REF_NONE);
    ok &= (blyt_buffer_ref(S_ENTITY, -1) == BLYT_ENTITY_REF_NONE);
    ok &= (blyt_buffer_ref(0, 0) == BLYT_ENTITY_REF_NONE);
    ok &= (blyt_buffer_ref(99, 0) == BLYT_ENTITY_REF_NONE);
    ok &= !blyt_buffer_ref_valid(S_ENTITY, BLYT_ENTITY_REF_NONE);

    int32_t g = -1, e0 = -1, e1 = -1;
    blyt_buffer_alloc_slot(S_GLOBALS, &g);
    blyt_buffer_alloc_slot(S_ENTITY, &e0);
    blyt_buffer_alloc_slot(S_ENTITY, &e1);
    ok &= (e0 == 0 && e1 == 1);
    /* slot 0, first generation: exact packed value */
    ok &= (blyt_buffer_ref(S_ENTITY, e0) == 0x00010000u);
    /* a ref to entity slot 1 is not valid against globals (count 1) */
    blyt_entity_ref_t r1 = blyt_buffer_ref(S_ENTITY, e1);
    ok &= blyt_buffer_ref_valid(S_ENTITY, r1);
    ok &= !blyt_buffer_ref_valid(S_GLOBALS, r1);
    ok &= !blyt_buffer_ref_valid(99, r1);

    blyt_console_debug(ok ? "edge_ok" : "edge_bad");
    blyt_quit();
}

void blyt_cart_draw(void) {}
"#;

#[test]
fn c_cart_entity_ref_edge_cases() {
    require_sdk();
    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("ref_edge_c");
    CartProject::new()
        .config(ENTITY_REF_CONFIG)
        .c(C_REF_EDGE)
        .write(&project);
    let cart = build_cart(&project);
    run_cart_native_with_flags(&cart, &[], "edge_ok");
}

#[test]
fn wasm_c_cart_entity_ref_edge_cases() {
    require_sdk();
    require_wasm();
    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("ref_edge_wasm");
    CartProject::new()
        .config(ENTITY_REF_CONFIG)
        .c(C_REF_EDGE)
        .write(&project);
    let cart = build_cart(&project);
    run_cart_wasm(&cart, "edge_ok");
}

#[test]
fn libretro_c_cart_entity_ref_edge_cases() {
    require_sdk();
    require_libretro_core();
    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("ref_edge_lr");
    CartProject::new()
        .config(ENTITY_REF_CONFIG)
        .c(C_REF_EDGE)
        .write(&project);
    let cart = build_cart(&project);
    run_cart_libretro(&cart, "edge_ok");
}

/// 65535 free/realloc cycles wrap the generation 65535 -> 1 (never 0): the
/// final ref equals the very first one and is still a valid, non-NONE ref.
#[test]
fn c_cart_entity_ref_gen_wrap() {
    require_sdk();
    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("ref_wrap_c");
    CartProject::new()
        .config(ENTITY_REF_CONFIG)
        .c(r#"
#include "blyt.h"
#include "cart_state.h"

void blyt_cart_init(void) {}

void blyt_cart_update(void) {
    int32_t s = -1;
    blyt_buffer_alloc_slot(S_ENTITY, &s);
    blyt_entity_ref_t first = blyt_buffer_ref(S_ENTITY, s); /* gen 1 */
    int ok = (first == 0x00010000u);
    for (uint32_t i = 0; i < 65535u; i++) {
        blyt_buffer_free_slot(S_ENTITY, s);
        blyt_buffer_alloc_slot(S_ENTITY, &s);
    }
    /* gen sequence 1 -> 2 -> ... -> 65535 -> 1 after 65535 frees */
    blyt_entity_ref_t wrapped = blyt_buffer_ref(S_ENTITY, s);
    ok &= (wrapped == first);
    ok &= (wrapped != BLYT_ENTITY_REF_NONE);
    ok &= blyt_buffer_ref_valid(S_ENTITY, wrapped);
    blyt_console_debug(ok ? "wrap_ok" : "wrap_bad");
    blyt_quit();
}

void blyt_cart_draw(void) {}
"#)
        .write(&project);
    let cart = build_cart(&project);
    run_cart_native_with_flags(&cart, &[], "wrap_ok");
}

/// Generations are serialized in save states: a ref taken before save_write
/// goes stale when its slot is freed, and becomes valid again after
/// save_read restores the generation counters and slot bitset.
const C_REF_SAVE: &str = r#"
#include "blyt.h"
#include "cart_state.h"

void blyt_cart_init(void) {
    int32_t g = -1, e = -1;
    blyt_buffer_alloc_slot(S_GLOBALS, &g);
    blyt_buffer_alloc_slot(S_ENTITY, &e);
    blyt_buffer_free_slot(S_ENTITY, e);
    blyt_buffer_alloc_slot(S_ENTITY, &e); /* gen 2 */
    blyt_buffer_set_u32(S_GLOBALS, 0, S_GLOBALS_TARGET, blyt_buffer_ref(S_ENTITY, e));
    blyt_save_write(0);
    blyt_buffer_free_slot(S_ENTITY, e); /* gen 3; target now stale */
}

void blyt_cart_update(void) {
    blyt_entity_ref_t target = blyt_buffer_get_u32(S_GLOBALS, 0, S_GLOBALS_TARGET);
    int ok = !blyt_buffer_ref_valid(S_ENTITY, target);
    blyt_save_read(0);
    target = blyt_buffer_get_u32(S_GLOBALS, 0, S_GLOBALS_TARGET);
    ok &= blyt_buffer_ref_valid(S_ENTITY, target);
    ok &= (target == ((2u << 16) | 0u));
    blyt_console_debug(ok ? "save_ref_ok" : "save_ref_bad");
    blyt_quit();
}

void blyt_cart_draw(void) {}
"#;

const LUA_REF_SAVE: &str = r#"
function init()
    blyt.buf.alloc_slot(S.GLOBALS)
    local e = blyt.buf.alloc_slot(S.ENTITY)
    blyt.buf.free_slot(S.ENTITY, e)
    e = blyt.buf.alloc_slot(S.ENTITY) -- gen 2
    S.globals[0].target = blyt.buf.ref(S.ENTITY, e)
    blyt.save_write(0)
    blyt.buf.free_slot(S.ENTITY, e) -- gen 3; target now stale
end

function update()
    local target = S.globals[0].target
    local ok = not blyt.buf.ref_valid(S.ENTITY, target)
    blyt.save_read(0)
    target = S.globals[0].target
    ok = ok and blyt.buf.ref_valid(S.ENTITY, target)
    ok = ok and (target == ((2 << 16) | 0))
    blyt.debug.print(ok and "save_ref_ok" or "save_ref_bad")
    blyt.quit()
end

function draw() end
"#;

#[test]
fn c_cart_ref_save_roundtrip() {
    require_sdk();
    let tmp = TempDir::new().unwrap();
    let save_dir = TempDir::new().unwrap();
    let project = tmp.path().join("ref_save_c");
    CartProject::new()
        .config(ENTITY_REF_CONFIG)
        .c(C_REF_SAVE)
        .write(&project);
    let cart = build_cart(&project);
    run_cart_native_with_env(
        &cart,
        &[("BLYT_SAVE_DIR", save_dir.path().to_str().unwrap())],
        "save_ref_ok",
    );
}

#[test]
fn lua_cart_ref_save_roundtrip() {
    require_sdk();
    require_lua_sdk();
    let tmp = TempDir::new().unwrap();
    let save_dir = TempDir::new().unwrap();
    let project = tmp.path().join("ref_save_lua");
    CartProject::new()
        .config(ENTITY_REF_CONFIG)
        .lua(LUA_REF_SAVE)
        .write(&project);
    let cart = build_lua_cart(&project);
    run_cart_native_with_env(
        &cart,
        &[("BLYT_SAVE_DIR", save_dir.path().to_str().unwrap())],
        "save_ref_ok",
    );
}

/// Exercises blyt.save_write/save_read + ref restore on the WASM host-Lua
/// fast path (wasm_main.c's save closures over the shared state_buffer.c).
#[test]
fn wasm_lua_cart_ref_save_roundtrip() {
    require_sdk();
    require_lua_sdk();
    require_wasm();
    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("ref_save_lua_wasm");
    CartProject::new()
        .config(ENTITY_REF_CONFIG)
        .lua(LUA_REF_SAVE)
        .write(&project);
    let cart = build_lua_cart(&project);
    run_cart_wasm_with_env(&cart, &[("BLYT_SAVE_DIR", "/tmp")], "save_ref_ok");
}

/// Generation counters are tracked state: a ref taken in on_new_state (with
/// a deliberate free/realloc so its generation is 2, not the fresh-boot 1)
/// must survive the per-frame snapshot/zero/init/restore cycle. If the cycle
/// dropped generations, the restored ref would read as stale.
const C_REF_RESET: &str = r#"
#include "blyt.h"
#include "cart_state.h"

void blyt_cart_init(void) {}

void blyt_cart_on_new_state(void) {
    int32_t g = -1, e = -1;
    blyt_buffer_alloc_slot(S_GLOBALS, &g);
    blyt_buffer_alloc_slot(S_ENTITY, &e);
    blyt_buffer_free_slot(S_ENTITY, e);
    blyt_buffer_alloc_slot(S_ENTITY, &e); /* gen 2 */
    blyt_buffer_set_u32(S_GLOBALS, 0, S_GLOBALS_TARGET, blyt_buffer_ref(S_ENTITY, e));
}

void blyt_cart_update(void) {
    int32_t frame = blyt_buffer_get_i32(S_GLOBALS, 0, S_GLOBALS_FRAME) + 1;
    blyt_buffer_set_i32(S_GLOBALS, 0, S_GLOBALS_FRAME, frame);
    if (frame == 3) {
        blyt_entity_ref_t target = blyt_buffer_get_u32(S_GLOBALS, 0, S_GLOBALS_TARGET);
        int ok = blyt_buffer_ref_valid(S_ENTITY, target);
        ok &= (target == ((2u << 16) | 0u));
        blyt_console_debug(ok ? "reset_ref_ok" : "reset_ref_bad");
        blyt_quit();
    }
}

void blyt_cart_draw(void) {}
"#;

#[test]
fn c_cart_entity_ref_reset_every_frame() {
    require_sdk();
    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("ref_reset_c");
    CartProject::new()
        .config(ENTITY_REF_CONFIG)
        .c(C_REF_RESET)
        .write(&project);
    let cart = build_cart(&project);
    run_cart_native_with_flags(&cart, &["--reset-every-frame"], "reset_ref_ok");
}

#[test]
fn wasm_c_cart_entity_ref_reset_every_frame() {
    require_sdk();
    require_wasm();
    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("ref_reset_wasm");
    CartProject::new()
        .config(ENTITY_REF_CONFIG)
        .c(C_REF_RESET)
        .write(&project);
    let cart = build_cart(&project);
    run_cart_wasm_with_env(&cart, &[("BLYT_RESET_EVERY_FRAME", "1")], "reset_ref_ok");
}

#[test]
fn libretro_c_cart_entity_ref_reset_every_frame() {
    require_sdk();
    require_libretro_core();
    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("ref_reset_lr");
    CartProject::new()
        .config(ENTITY_REF_CONFIG)
        .c(C_REF_RESET)
        .write(&project);
    let cart = build_cart(&project);
    run_cart_libretro_with_flags(&cart, &["--reset-every-frame"], "reset_ref_ok");
}

/// A `ref:` field whose target buffer is not declared fails `blyt build`
/// with a pointed error message.
#[test]
fn ref_field_unknown_buffer_fails_build() {
    require_sdk();
    let tmp = TempDir::new().unwrap();
    let project = tmp.path().join("ref_bad_target");
    CartProject::new()
        .config(
            "\
records:
  Globals:
    fields:
      - { name: target, ref: nonexistent }
state_buffers:
  globals:
    record: Globals
    count: 1
",
        )
        .c(r#"
#include "blyt.h"
void blyt_cart_init(void) {}
void blyt_cart_update(void) { blyt_quit(); }
void blyt_cart_draw(void) {}
"#)
        .write(&project);

    let sdk = common::sdk_dir();
    let out = std::process::Command::new(common::blyt_bin())
        .args(["build", project.to_str().unwrap()])
        .env("BLYT_SDK_DIR", &sdk)
        .output()
        .expect("failed to spawn blyt build");
    assert!(!out.status.success(), "build unexpectedly succeeded");
    let stderr = String::from_utf8_lossy(&out.stderr);
    assert!(
        stderr.contains("ref target buffer") && stderr.contains("nonexistent"),
        "unexpected error output: {stderr}"
    );
}
