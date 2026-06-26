//! Text-resource API (issue #94) — the Rust ergonomics layer over the resource
//! lifecycle ECALLs (`blyt_resource_load`/`release`/`pin`/`unpin`, issue #123).
//!
//! Carts never name assets by path at runtime: the packer assigns each asset an
//! integer id and emits an `R_<NAME>: ResourceHandle` constant in the generated
//! `cart_resources.rs`, pulled in with
//! `include!(env!("BLYT_CART_RESOURCES_RS"))`.
//!
//! Two lifecycle layers (ADR-0027):
//!   - [`load`]/[`TextHandle::release`] — residency/caching hints.  `load`
//!     returns a [`TextHandle`] that is stable across frames; `release` (also
//!     run by `Drop`) tells the runtime the cart is done with it.
//!   - [`pin`] — a **within-frame** raw-access window.  The [`PinnedResource`]
//!     guard borrows the runtime's scratch copy of the bytes; the borrow is
//!     valid only for the frame the pin was taken (the runtime force-releases
//!     pins at the frame boundary, because dev-mode hot-reload may move the
//!     bytes between frames).  The guard's borrows are tied to its lifetime, so
//!     a `&str`/`&[u8]` from a pin cannot outlive it; do not hold a
//!     `PinnedResource` across a frame.  Use [`TextHandle::text_string`] for the
//!     owned, cross-frame copy.
//!
//! Error model (ADR-0108, two-tier): [`load`] and [`pin`] take no `Result` —
//! loading an asset from the cart's own bundle cannot fail in a correct cart, so
//! a failure is a programming error that panics in debug builds and is elided in
//! release.  Content errors (bytes that are not valid UTF-8) are a different
//! class: [`PinnedResource::as_str`] returns a `Result`, and
//! [`TextHandle::text_string`] panics.

extern crate alloc;

use alloc::string::String;
use core::cell::Cell;
use core::ffi::c_void;
use core::str::Utf8Error;

// -------------------------------------------------------------------------
// Raw C ECALL surface (runtime/guest/include/blyt.h; blyt_result_t == 0 is OK).
// -------------------------------------------------------------------------

extern "C" {
    fn blyt_resource_load(id: u32, out_handle: *mut u32) -> u32;
    fn blyt_resource_release(handle: u32) -> u32;
    fn blyt_resource_pin(id: u32, out_ptr: *mut *const c_void, out_size: *mut usize) -> u32;
    fn blyt_resource_unpin(id: u32) -> u32;
}

/// Sentinel for an invalid load handle (`BLYT_RESOURCE_INVALID`).
const RESOURCE_INVALID: u32 = 0;

/// A packer-assigned resource id — the type of the generated `R_<NAME>`
/// constants.  A distinct type per ADR-0108: passing a `ResourceHandle` where a
/// future `ImageHandle` is expected is a compile error.  The wrapped `u32` is
/// the integer id the packer assigned the asset.
#[derive(Copy, Clone, PartialEq, Eq, Debug)]
#[repr(transparent)]
pub struct ResourceHandle(u32);

impl ResourceHandle {
    /// Construct a handle from a packer-assigned id.  `const` so the generated
    /// `R_<NAME>` constants are usable in const context.
    pub const fn new(id: u32) -> Self {
        ResourceHandle(id)
    }

    /// The underlying packer-assigned integer id.
    pub const fn id(self) -> u32 {
        self.0
    }
}

/// A loaded text resource (returned by [`load`]).
///
/// The handle keeps the resource resident.  `Drop` calls [`release`]
/// automatically; call it explicitly at scene transitions to free large
/// resources sooner.
///
/// [`release`]: TextHandle::release
pub struct TextHandle {
    id: u32,
    handle: u32,
    released: Cell<bool>,
}

impl TextHandle {
    /// Copy the resource's bytes into an owned `String`.  Unlike a [`pin`], the
    /// copy outlives the current frame.
    ///
    /// Panics if the bytes are not valid UTF-8 (a content error in the asset).
    pub fn text_string(&self) -> String {
        let pinned = pin(ResourceHandle(self.id));
        String::from_utf8(pinned.as_bytes().into())
            .expect("blyt: resource bytes are not valid UTF-8")
    }

    /// Advisory release: tell the runtime the cart no longer needs this
    /// resource.  Idempotent, and also run by `Drop`.
    pub fn release(&self) {
        if !self.released.replace(true) {
            // SAFETY: the handle came from a successful `load`; a stale handle
            // is rejected by the runtime, so this is safe even if called twice.
            unsafe {
                blyt_resource_release(self.handle);
            }
        }
    }
}

impl Drop for TextHandle {
    fn drop(&mut self) {
        self.release();
    }
}

/// A within-frame borrow of a resource's bytes (returned by [`pin`]).
///
/// The borrows returned by [`as_str`]/[`as_bytes`]/[`as_raw`] are tied to this
/// guard's lifetime and are valid only for the frame the pin was taken.  `Drop`
/// decrements the pin count.  Do not hold a `PinnedResource` across a frame.
///
/// [`as_str`]: PinnedResource::as_str
/// [`as_bytes`]: PinnedResource::as_bytes
/// [`as_raw`]: PinnedResource::as_raw
pub struct PinnedResource {
    id: u32,
    ptr: *const c_void,
    size: usize,
}

impl PinnedResource {
    /// Borrow the pinned bytes.
    pub fn as_bytes(&self) -> &[u8] {
        if self.ptr.is_null() || self.size == 0 {
            return &[];
        }
        // SAFETY: a successful pin gave us a `size`-byte region valid for this
        // frame; the returned slice is bounded by `&self`, so it cannot outlive
        // the guard (and thus the frame).
        unsafe { core::slice::from_raw_parts(self.ptr as *const u8, self.size) }
    }

    /// Borrow the pinned bytes as `&str`, validating UTF-8.
    pub fn as_str(&self) -> Result<&str, Utf8Error> {
        core::str::from_utf8(self.as_bytes())
    }

    /// The raw pointer and byte length, for passing to a C API.  The pointer is
    /// valid only while this guard is alive (the current frame).
    pub fn as_raw(&self) -> (*const c_void, usize) {
        (self.ptr, self.size)
    }
}

impl Drop for PinnedResource {
    fn drop(&mut self) {
        if !self.ptr.is_null() {
            // SAFETY: balances the pin taken in `pin`; the runtime rejects an
            // unbalanced unpin, so this is safe.
            unsafe {
                blyt_resource_unpin(self.id);
            }
        }
    }
}

/// Load a resource and keep it resident.  Idempotent (re-loading a resident
/// resource returns an equivalent handle).
///
/// Tier 2 (ADR-0108): no `Result` — a missing asset is a broken cart, so this
/// panics in debug builds and is elided in release.
pub fn load(handle: ResourceHandle) -> TextHandle {
    let mut out: u32 = RESOURCE_INVALID;
    // SAFETY: `out` is a valid, writable u32.
    let rc = unsafe { blyt_resource_load(handle.0, &mut out) };
    debug_assert!(
        rc == 0 && out != RESOURCE_INVALID,
        "blyt: resource load failed (id={}, rc={})",
        handle.0,
        rc
    );
    TextHandle {
        id: handle.0,
        handle: out,
        released: Cell::new(false),
    }
}

/// Pin a resource for within-frame raw access, returning a [`PinnedResource`]
/// guard.  Multiple concurrent pins are valid; each is dropped independently.
///
/// Tier 2 (ADR-0108): no `Result`, same rationale as [`load`].
pub fn pin(handle: ResourceHandle) -> PinnedResource {
    let mut ptr: *const c_void = core::ptr::null();
    let mut size: usize = 0;
    // SAFETY: `ptr` and `size` are valid, writable out-params.
    let rc = unsafe { blyt_resource_pin(handle.0, &mut ptr, &mut size) };
    debug_assert!(
        rc == 0 && !ptr.is_null(),
        "blyt: resource pin failed (id={}, rc={})",
        handle.0,
        rc
    );
    PinnedResource {
        id: handle.0,
        ptr,
        size,
    }
}
