#![no_std]
extern crate alloc;
use alloc::format;
use core::sync::atomic::{AtomicI32, AtomicU32, Ordering};
include!(env!("BLYT_CART_STATE_RS"));

use blyt::buffer::{alloc_slot, get_i32, set_i32};

static S_FRAME: AtomicU32 = AtomicU32::new(0);
static S_SLOT: AtomicI32 = AtomicI32::new(-1);

#[no_mangle]
pub extern "C" fn blyt_cart_init() {
    alloc_slot(S_GLOBALS);
    let slot = alloc_slot(S_PLAYER);
    S_SLOT.store(slot, Ordering::Relaxed);
    set_i32(S_PLAYER, slot, S_PLAYER_X, 0);
    set_i32(S_PLAYER, slot, S_PLAYER_Y, 0);
    blyt::console_debug("init player pos: 0, 0");
}

#[no_mangle]
pub extern "C" fn blyt_cart_update() {
    let frame = S_FRAME.fetch_add(1, Ordering::Relaxed) + 1;
    if frame % 10 == 0 {
        let slot = S_SLOT.load(Ordering::Relaxed);
        let x = (get_i32(S_PLAYER, slot, S_PLAYER_X) + 1) % 320;
        let y = (get_i32(S_PLAYER, slot, S_PLAYER_Y) + 1) % 240;
        set_i32(S_PLAYER, slot, S_PLAYER_X, x);
        set_i32(S_PLAYER, slot, S_PLAYER_Y, y);
        blyt::console_debug(&format!("update frame {} player pos: {}, {}", frame, x, y));
    }
}

#[no_mangle]
pub extern "C" fn blyt_cart_draw() {
    let frame = S_FRAME.load(Ordering::Relaxed);
    if frame % 10 == 0 {
        let slot = S_SLOT.load(Ordering::Relaxed);
        let x = get_i32(S_PLAYER, slot, S_PLAYER_X);
        let y = get_i32(S_PLAYER, slot, S_PLAYER_Y);
        blyt::console_debug(&format!("draw frame {} player pos: {}, {}", frame, x, y));
    }
}

#[no_mangle]
pub extern "C" fn blyt_cart_on_save_state() {
    set_i32(
        S_GLOBALS,
        0,
        S_GLOBALS_FRAME,
        S_FRAME.load(Ordering::Relaxed) as i32,
    );
}

#[no_mangle]
pub extern "C" fn blyt_cart_on_load_state(_reason: u32, _saved_version: u32, _buffers: u32) {
    S_FRAME.store(
        get_i32(S_GLOBALS, 0, S_GLOBALS_FRAME) as u32,
        Ordering::Relaxed,
    );
    S_SLOT.store(0, Ordering::Relaxed);
}
