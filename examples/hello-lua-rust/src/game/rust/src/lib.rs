#![no_std]

extern crate blyt;
use blyt::lua::{api, lua_export, LuaState};

#[lua_export(module = "greeting", raw)]
fn log(l: LuaState) {
    let (s, _len) = api::check_lstring(l, 1);
    blyt::console_debug_ptr(s);
}
