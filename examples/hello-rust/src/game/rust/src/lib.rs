#![no_std]
extern crate alloc;
use alloc::format;
use core::sync::atomic::{AtomicU32, Ordering};

static S_FRAME: AtomicU32 = AtomicU32::new(0);

#[no_mangle]
pub extern "C" fn blyt_cart_init() {
    blyt::console_debug("hello from rust");
}

#[no_mangle]
pub extern "C" fn blyt_cart_update() {
    let frame = S_FRAME.fetch_add(1, Ordering::Relaxed) + 1;
    if frame % 60 == 0 {
        blyt::console_debug(&format!("update {}", frame));
    }
}

#[no_mangle]
pub extern "C" fn blyt_cart_draw() {
    let frame = S_FRAME.load(Ordering::Relaxed);
    if frame % 60 == 0 {
        blyt::console_debug(&format!("draw {}", frame));
    }
}
