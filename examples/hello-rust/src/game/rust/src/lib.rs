#![no_std]
extern crate alloc;
use alloc::format;
use core::cell::Cell;
include!(env!("BLYT_CART_STATE_RS"));

use blyt::buffer::{alloc_slot, entity_ref, get_i32, get_u32, ref_slot, ref_valid, set_i32, set_u32};

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

/// S_FRAME is deliberately plain static state (not a state buffer) to
/// demonstrate serialising static state in on_save_state/on_load_state.
static S_FRAME: BlytCell<u32> = BlytCell::new(0);

#[no_mangle]
pub extern "C" fn blyt_cart_init() {
    S_FRAME.set(0);
}

#[no_mangle]
pub extern "C" fn blyt_cart_on_new_state() {
    alloc_slot(S_GLOBALS);
    let slot = alloc_slot(S_CHARACTER);
    set_u32(S_GLOBALS, 0, S_GLOBALS_PLAYER, entity_ref(S_CHARACTER, slot));
    set_i32(S_CHARACTER, slot, S_CHARACTER_X, 160);
    set_i32(S_CHARACTER, slot, S_CHARACTER_Y, 120);
    blyt::console_debug("init player pos: 160, 120");
}

#[no_mangle]
pub extern "C" fn blyt_cart_update() {
    let frame = S_FRAME.get() + 1;
    S_FRAME.set(frame);
    if frame % 10 == 0 {
        let player = get_u32(S_GLOBALS, 0, S_GLOBALS_PLAYER);
        if ref_valid(S_CHARACTER, player) {
            let slot = ref_slot(player);
            let x = (get_i32(S_CHARACTER, slot, S_CHARACTER_X) + 1) % 320;
            let y = (get_i32(S_CHARACTER, slot, S_CHARACTER_Y) + 1) % 240;
            set_i32(S_CHARACTER, slot, S_CHARACTER_X, x);
            set_i32(S_CHARACTER, slot, S_CHARACTER_Y, y);
            blyt::console_debug(&format!("update frame {} player pos: {}, {}", frame, x, y));
        }
    }
}

#[no_mangle]
pub extern "C" fn blyt_cart_draw() {
    let frame = S_FRAME.get();
    if frame % 10 == 0 {
        let slot = ref_slot(get_u32(S_GLOBALS, 0, S_GLOBALS_PLAYER));
        let x = get_i32(S_CHARACTER, slot, S_CHARACTER_X);
        let y = get_i32(S_CHARACTER, slot, S_CHARACTER_Y);
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
}
