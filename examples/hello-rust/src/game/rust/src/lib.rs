#![no_std]
extern crate alloc;
use alloc::format;
use core::cell::Cell;
include!(env!("BLYT_CART_STATE_RS"));

use blyt::buffer::{alloc_slot, get_i32, set_i32};

/// A mutable global for the blyt cart environment.
///
/// SAFETY: the `unsafe impl Sync` below is sound because the blyt runtime
/// executes cart callbacks on a single thread, so there is never concurrent
/// access to synchronize. This is the only `unsafe` in the cart.
#[repr(transparent)]
struct BlytCell<T>(Cell<T>);
unsafe impl<T> Sync for BlytCell<T> {}

impl<T: Copy> BlytCell<T> {
    const fn new(v: T) -> Self {
        Self(Cell::new(v))
    }
    fn get(&self) -> T {
        self.0.get()
    }
    fn set(&self, v: T) {
        self.0.set(v)
    }
    #[allow(dead_code)]
    fn replace(&self, v: T) -> T {
        self.0.replace(v)
    }
}

static S_FRAME: BlytCell<u32> = BlytCell::new(0);
static S_SLOT: BlytCell<i32> = BlytCell::new(-1);

#[no_mangle]
pub extern "C" fn blyt_cart_init() {
    alloc_slot(S_GLOBALS);
    let slot = alloc_slot(S_PLAYER);
    S_SLOT.set(slot);
}

#[no_mangle]
pub extern "C" fn blyt_cart_on_new_state() {
    let slot = S_SLOT.get();
    set_i32(S_PLAYER, slot, S_PLAYER_X, 160);
    set_i32(S_PLAYER, slot, S_PLAYER_Y, 120);
    blyt::console_debug("init player pos: 160, 120");
}

#[no_mangle]
pub extern "C" fn blyt_cart_update() {
    let frame = S_FRAME.get() + 1;
    S_FRAME.set(frame);
    if frame % 10 == 0 {
        let slot = S_SLOT.get();
        let x = (get_i32(S_PLAYER, slot, S_PLAYER_X) + 1) % 320;
        let y = (get_i32(S_PLAYER, slot, S_PLAYER_Y) + 1) % 240;
        set_i32(S_PLAYER, slot, S_PLAYER_X, x);
        set_i32(S_PLAYER, slot, S_PLAYER_Y, y);
        blyt::console_debug(&format!("update frame {} player pos: {}, {}", frame, x, y));
    }
}

#[no_mangle]
pub extern "C" fn blyt_cart_draw() {
    let frame = S_FRAME.get();
    if frame % 10 == 0 {
        let slot = S_SLOT.get();
        let x = get_i32(S_PLAYER, slot, S_PLAYER_X);
        let y = get_i32(S_PLAYER, slot, S_PLAYER_Y);
        blyt::console_debug(&format!("draw frame {} player pos: {}, {}", frame, x, y));
    }
}

#[no_mangle]
pub extern "C" fn blyt_cart_on_save_state() {
    set_i32(S_GLOBALS, 0, S_GLOBALS_FRAME, S_FRAME.get() as i32);
}

#[no_mangle]
pub extern "C" fn blyt_cart_on_load_state(_reason: u32, _saved_version: u32, _buffers: u32) {
    S_FRAME.set(get_i32(S_GLOBALS, 0, S_GLOBALS_FRAME) as u32);
    S_SLOT.set(0);
}
