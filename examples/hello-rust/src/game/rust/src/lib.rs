#![no_std]
extern crate alloc;
use alloc::format;

static mut S_FRAME: u32 = 0;

#[no_mangle]
pub extern "C" fn blyt_cart_init() {
    blyt::console_debug("hello from rust");
}

#[no_mangle]
pub extern "C" fn blyt_cart_update() {
    unsafe {
        S_FRAME += 1;
        if S_FRAME % 60 == 0 {
            blyt::console_debug(&format!("update {}", S_FRAME));
        }
    }
}

#[no_mangle]
pub extern "C" fn blyt_cart_draw() {
    unsafe {
        if S_FRAME % 60 == 0 {
            blyt::console_debug(&format!("draw {}", S_FRAME));
        }
    }
}
