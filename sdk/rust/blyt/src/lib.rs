//! Blyt cart SDK — Rust bindings for the blyt runtime API.
//!
//! `#![no_std]`.  The `blyt` crate provides:
//!
//! - Safe wrappers around the C API declared in `blyt.h`
//! - A `#[global_allocator]` backed by `libblytc`'s `malloc`/`free`
//! - A `#[panic_handler]` that calls `abort()` from `libblytc`
//!
//! Cart code must not define its own panic handler or global allocator.

#![no_std]

use core::ffi::c_char;

// -------------------------------------------------------------------------
// Raw C API
// -------------------------------------------------------------------------

extern "C" {
    fn blyt_console_debug(s: *const c_char);
    fn blyt_quit();
    fn blyt_frame_done();
    // libblytc allocator
    fn malloc(size: usize) -> *mut core::ffi::c_void;
    fn free(ptr: *mut core::ffi::c_void);
    fn realloc(ptr: *mut core::ffi::c_void, size: usize) -> *mut core::ffi::c_void;
    fn abort() -> !;
}

// -------------------------------------------------------------------------
// Public cart API
// -------------------------------------------------------------------------

/// Write a debug message to the host console.
///
/// `s` is truncated to 255 bytes if longer; null bytes in the middle of `s`
/// terminate the output early (C string semantics in the host).
pub fn console_debug(s: &str) {
    let mut buf = [0u8; 256];
    let len = s.len().min(255);
    buf[..len].copy_from_slice(&s.as_bytes()[..len]);
    // buf[len] is 0 from zero-initialisation — null terminator.
    unsafe { blyt_console_debug(buf.as_ptr().cast()) }
}

/// Signal the runtime that the cart is ready to exit.
///
/// Call from `blyt_cart_update` when the cart decides it has finished.
/// The runtime will stop calling `blyt_cart_update` and `blyt_cart_draw`
/// after the current frame and proceed with teardown.
pub fn quit() {
    unsafe { blyt_quit() }
}

/// Signal the end of one update+draw frame.
///
/// Normally called automatically by `libblytcommon` after each
/// `blyt_cart_draw()`; cart code does not need to call this directly.
pub fn frame_done() {
    unsafe { blyt_frame_done() }
}

// -------------------------------------------------------------------------
// Global allocator — backed by libblytc malloc/free
// -------------------------------------------------------------------------

use core::alloc::{GlobalAlloc, Layout};

struct BlytAlloc;

unsafe impl GlobalAlloc for BlytAlloc {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        // libblytc malloc guarantees pointer-size alignment.  Over-aligned
        // layouts are not yet handled; alloc returns null and the caller
        // will hit the OOM path.  Sufficient for Phase 7 heap usage.
        unsafe { malloc(layout.size()) as *mut u8 }
    }

    unsafe fn dealloc(&self, ptr: *mut u8, _layout: Layout) {
        unsafe { free(ptr as *mut core::ffi::c_void) }
    }

    unsafe fn realloc(&self, ptr: *mut u8, _layout: Layout, new_size: usize) -> *mut u8 {
        unsafe { realloc(ptr as *mut core::ffi::c_void, new_size) as *mut u8 }
    }
}

#[global_allocator]
static ALLOC: BlytAlloc = BlytAlloc;

// -------------------------------------------------------------------------
// Panic handler — abort via libblytc
// -------------------------------------------------------------------------

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    unsafe { abort() }
}

// -------------------------------------------------------------------------
// Lua export support (feature "lua") — enabled for Lua+Rust hybrid carts.
//
// Provides cart_lua_modules (iterates .lua_regtab populated by #[lua_export])
// and re-exports the lua_export attribute macro from blyt-lua-macros.
// -------------------------------------------------------------------------

#[cfg(feature = "lua")]
pub mod lua {
    pub use blyt_lua_macros::lua_export;

    // Matches blyt_lua_export_entry_t in runtime/guest/include/blyt.h.
    #[repr(C)]
    pub struct BlytLuaExportEntry {
        pub lua_name:  [u8; 32],
        pub fn_sym:    [u8; 64],
        pub wrap_sym:  [u8; 64],
        pub nargs:     u8,
        pub arg_types: [u8; 4],
        pub ret_type:  u8,
        pub pad:       [u8; 2],
    }

    pub const LUA_TYPE_VOID: u8 = 0;
    pub const LUA_TYPE_I32:  u8 = 1;
    pub const LUA_TYPE_U32:  u8 = 2;
    pub const LUA_TYPE_F32:  u8 = 3;
    pub const LUA_TYPE_BOOL: u8 = 4;

    // Used by proc-macro generated statics to build BlytLuaExportEntry at
    // compile time without requiring heap allocation.
    pub const fn str_to_fixed<const N: usize>(s: &str) -> [u8; N] {
        let src = s.as_bytes();
        let mut arr = [0u8; N];
        let mut i = 0;
        while i < src.len() && i + 1 < N {
            arr[i] = src[i];
            i += 1;
        }
        arr
    }

    type RegFn = unsafe extern "C" fn(*mut ::core::ffi::c_void);

    // __start_lua_regtab / __stop_lua_regtab are defined via PROVIDE in
    // HYBRID_LUA_LINKER_SCRIPT.  PROVIDE-defined symbols can be accessed
    // through the GOT in PIE mode (unlike synthesized start/stop symbols).
    extern "C" {
        static __start_lua_regtab: RegFn;
        static __stop_lua_regtab:  RegFn;
    }

    #[no_mangle]
    pub unsafe extern "C" fn cart_lua_modules(l: *mut ::core::ffi::c_void) {
        let start = ::core::ptr::addr_of!(__start_lua_regtab) as *const RegFn;
        let end   = ::core::ptr::addr_of!(__stop_lua_regtab)  as *const RegFn;
        let mut p = start;
        while p < end {
            unsafe { (*p)(l) };
            p = unsafe { p.add(1) };
        }
    }
}

#[cfg(feature = "lua")]
pub use lua::lua_export;
