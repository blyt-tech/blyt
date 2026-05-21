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
    fn blyt_quit_ready();
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
pub fn quit_ready() {
    unsafe { blyt_quit_ready() }
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
