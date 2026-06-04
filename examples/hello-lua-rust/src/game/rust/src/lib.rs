#![no_std]

// The blyt crate provides the global allocator and panic handler required by
// staticlib crates on this target.
extern crate blyt as _;

use core::ffi::{c_char, c_int, c_void};

type LuaState = c_void;

extern "C" {
    fn blyt_console_debug(s: *const c_char);
    fn lua_createtable(L: *mut LuaState, narr: c_int, nrec: c_int);
    fn lua_pushcclosure(
        L: *mut LuaState,
        f: unsafe extern "C" fn(*mut LuaState) -> c_int,
        n: c_int,
    );
    fn lua_setfield(L: *mut LuaState, idx: c_int, k: *const c_char);
    fn luaL_checklstring(L: *mut LuaState, arg: c_int, l: *mut usize) -> *const c_char;
    fn luaL_requiref(
        L: *mut LuaState,
        modname: *const c_char,
        openf: unsafe extern "C" fn(*mut LuaState) -> c_int,
        glb: c_int,
    );
    fn lua_settop(L: *mut LuaState, idx: c_int);
}

unsafe extern "C" fn l_log(L: *mut LuaState) -> c_int {
    let s = luaL_checklstring(L, 1, core::ptr::null_mut());
    blyt_console_debug(s);
    0
}

unsafe extern "C" fn luaopen_greeting(L: *mut LuaState) -> c_int {
    lua_createtable(L, 0, 1);
    lua_pushcclosure(L, l_log, 0);
    lua_setfield(L, -2, b"log\0".as_ptr() as *const c_char);
    1
}

#[no_mangle]
pub unsafe extern "C" fn cart_lua_modules(L: *mut LuaState) {
    luaL_requiref(
        L,
        b"greeting\0".as_ptr() as *const c_char,
        luaopen_greeting,
        1,
    );
    lua_settop(L, -2); // lua_pop(L, 1)
}
