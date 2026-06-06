#![no_std]

extern crate blyt;
use blyt::lua_export;
use core::ffi::c_char;

extern "C" {
    fn blyt_console_debug(s: *const c_char);
}

#[lua_export(module = "greeting")]
fn hello() {
    unsafe { blyt_console_debug(b"hello from lua+rust\0".as_ptr() as *const c_char) };
}
