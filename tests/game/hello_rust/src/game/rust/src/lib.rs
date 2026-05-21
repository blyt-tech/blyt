#![no_std]

#[no_mangle]
pub extern "C" fn blyt_cart_init() {
    blyt::console_debug("hello from rust");
}

#[no_mangle]
pub extern "C" fn blyt_cart_update() {
    blyt::quit_ready();
}

#[no_mangle]
pub extern "C" fn blyt_cart_draw() {}
