#![no_std]

extern crate blyt;
use blyt::lua::{capi::luaL_checklstring, lua_export, LuaState};
use core::ffi::c_char;

extern "C" {
    fn blyt_console_debug(s: *const c_char);
}

#[lua_export(module = "greeting", raw)]
fn log(l: LuaState) {
    unsafe {
        let s = luaL_checklstring(l, 1, core::ptr::null_mut());
        blyt_console_debug(s);
    }
}
