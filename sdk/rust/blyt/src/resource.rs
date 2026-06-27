//! Typed resource API (issues #94, #166) — the Rust ergonomics layer over the
//! resource lifecycle ECALLs (`blyt_resource_load`/`release`/`pin`/`unpin`,
//! issue #123).
//!
//! Carts never name assets by path at runtime: the packer assigns each asset an
//! integer id and emits a typed `R_<NAME>` constant in the generated
//! `cart_resources.rs`, pulled in with
//! `include!(env!("BLYT_CART_RESOURCES_RS"))`. The constant is a
//! [`TextResource`] for a `text` asset or a [`BytesResource`] for a `raw` one
//! (ADR-0068 amendment 2026-06-27, #166) — concrete newtypes, not a phantom
//! `Handle<Kind>`, so the text accessor exists only on the text types and
//! misusing it is a compile error.
//!
//! Two lifecycle layers (ADR-0027):
//!   - `R_X.load()` → a `Loaded*` handle (residency/caching hints). The handle
//!     is stable across frames; `release` (also run by `Drop`) tells the runtime
//!     the cart is done with it.
//!   - `R_X.pin()` → a `Pinned*` guard: a **within-frame** raw-access window.
//!     The guard borrows the runtime's scratch copy of the bytes; the borrow is
//!     valid only for the frame the pin was taken (the runtime force-releases
//!     pins at the frame boundary, because dev-mode hot-reload may move the
//!     bytes between frames). The guard's borrows are tied to its lifetime, so a
//!     `&str`/`&[u8]` from a pin cannot outlive it; do not hold a `Pinned*`
//!     across a frame. Use [`LoadedText::text_string`]/[`LoadedBytes::bytes_vec`]
//!     for the owned, cross-frame copy.
//!
//! Text storage (#166): a `text` resource is stored with a build-appended
//! trailing NUL; the runtime is byte-blind and reports the stored length
//! *including* it. The text accessors ([`PinnedText::as_str`],
//! [`LoadedText::text_string`]) strip that NUL, so a `String` / `&str` has the
//! correct content length. `BytesResource` stays byte-exact.
//!
//! Error model (ADR-0108, two-tier): `load`/`pin` take no `Result` — loading an
//! asset from the cart's own bundle cannot fail in a correct cart, so a failure
//! is a programming error that panics in debug builds and is elided in release.
//! Content errors (text bytes that are not valid UTF-8) are a different class:
//! [`PinnedText::as_str`] returns a `Result`, and [`LoadedText::text_string`]
//! panics.

extern crate alloc;

use alloc::string::String;
use alloc::vec::Vec;
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

/// Mark a resource resident; returns `(id, load_handle)`. Tier 2 (ADR-0108): no
/// `Result` — a missing asset is a broken cart, so this panics in debug and is
/// elided in release.
fn load_raw(id: u32) -> (u32, u32) {
    let mut out: u32 = RESOURCE_INVALID;
    // SAFETY: `out` is a valid, writable u32.
    let rc = unsafe { blyt_resource_load(id, &mut out) };
    debug_assert!(
        rc == 0 && out != RESOURCE_INVALID,
        "blyt: resource load failed (id={id}, rc={rc})"
    );
    (id, out)
}

/// Pin a resource for within-frame raw access; returns `(ptr, size)`. `size` is
/// the stored byte length (includes the trailing NUL for text resources).
fn pin_raw(id: u32) -> (*const c_void, usize) {
    let mut ptr: *const c_void = core::ptr::null();
    let mut size: usize = 0;
    // SAFETY: `ptr` and `size` are valid, writable out-params.
    let rc = unsafe { blyt_resource_pin(id, &mut ptr, &mut size) };
    debug_assert!(
        rc == 0 && !ptr.is_null(),
        "blyt: resource pin failed (id={id}, rc={rc})"
    );
    (ptr, size)
}

/// Generate a concrete typed-handle family (constant newtype + loaded handle +
/// pin guard) for one resource kind. The lifecycle boilerplate is identical
/// across kinds (ADR-0068 amendment 2026-06-27 rationale: future image/voice/…
/// handles are concrete newtypes too, so a macro keeps the family uniform
/// without hand-duplication); the kind-specific accessors are added in separate
/// `impl` blocks below.
macro_rules! resource_kind {
    (
        $(#[$res_doc:meta])* resource: $Res:ident,
        $(#[$loaded_doc:meta])* loaded: $Loaded:ident,
        $(#[$pinned_doc:meta])* pinned: $Pinned:ident $(,)?
    ) => {
        $(#[$res_doc])*
        #[derive(Copy, Clone, PartialEq, Eq, Debug)]
        #[repr(transparent)]
        pub struct $Res(u32);

        impl $Res {
            /// Construct a typed resource constant from a packer-assigned id.
            /// `const` so the generated `R_<NAME>` constants are usable in const
            /// context.
            pub const fn new(id: u32) -> Self {
                $Res(id)
            }

            /// The underlying packer-assigned integer id.
            pub const fn id(self) -> u32 {
                self.0
            }

            /// Load the resource and keep it resident, returning a handle that
            /// is stable across frames.
            pub fn load(self) -> $Loaded {
                let (id, handle) = load_raw(self.0);
                $Loaded { id, handle, released: Cell::new(false) }
            }

            /// Pin the resource for within-frame raw access.
            pub fn pin(self) -> $Pinned {
                let (ptr, size) = pin_raw(self.0);
                $Pinned { id: self.0, ptr, size }
            }
        }

        $(#[$loaded_doc])*
        pub struct $Loaded {
            id: u32,
            handle: u32,
            released: Cell<bool>,
        }

        impl $Loaded {
            /// Pin this resource for within-frame raw access.
            pub fn pin(&self) -> $Pinned {
                let (ptr, size) = pin_raw(self.id);
                $Pinned { id: self.id, ptr, size }
            }

            /// Advisory release: tell the runtime the cart no longer needs this
            /// resource. Idempotent, and also run by `Drop`.
            pub fn release(&self) {
                if !self.released.replace(true) {
                    // SAFETY: the handle came from a successful `load`; a stale
                    // handle is rejected by the runtime, so this is safe even if
                    // called twice.
                    unsafe {
                        blyt_resource_release(self.handle);
                    }
                }
            }
        }

        impl Drop for $Loaded {
            fn drop(&mut self) {
                self.release();
            }
        }

        $(#[$pinned_doc])*
        pub struct $Pinned {
            id: u32,
            ptr: *const c_void,
            size: usize,
        }

        impl $Pinned {
            /// The raw stored bytes (text resources include the trailing NUL).
            fn raw(&self) -> &[u8] {
                if self.ptr.is_null() || self.size == 0 {
                    return &[];
                }
                // SAFETY: a successful pin gave us a `size`-byte region valid for
                // this frame; the slice is bounded by `&self`, so it cannot
                // outlive the guard (and thus the frame).
                unsafe { core::slice::from_raw_parts(self.ptr as *const u8, self.size) }
            }
        }

        impl Drop for $Pinned {
            fn drop(&mut self) {
                if !self.ptr.is_null() {
                    // SAFETY: balances the pin taken in `pin`; the runtime
                    // rejects an unbalanced unpin, so this is safe.
                    unsafe {
                        blyt_resource_unpin(self.id);
                    }
                }
            }
        }
    };
}

resource_kind! {
    /// A `text` resource constant (the generated `R_<NAME>`). Loading or pinning
    /// it yields the text-only accessors; the bytes accessors do not exist on
    /// it, so misuse is a compile error.
    resource: TextResource,
    /// A loaded text resource. `Drop` calls `release` automatically; call it
    /// explicitly at scene transitions to free large resources sooner.
    loaded: LoadedText,
    /// A within-frame borrow of a text resource's bytes. The `&str` from
    /// [`as_str`](PinnedText::as_str) is tied to this guard's lifetime and valid
    /// only for the frame the pin was taken.
    pinned: PinnedText,
}

resource_kind! {
    /// A `raw`/opaque resource constant (the generated `R_<NAME>`). Loading or
    /// pinning it yields the byte-exact accessors; the text accessor does not
    /// exist on it.
    resource: BytesResource,
    /// A loaded raw resource. `Drop` calls `release` automatically.
    loaded: LoadedBytes,
    /// A within-frame borrow of a raw resource's exact bytes.
    pinned: PinnedBytes,
}

impl LoadedText {
    /// Copy the resource's content into an owned `String` (the trailing storage
    /// NUL is stripped). Unlike a [`pin`](LoadedText::pin), the copy outlives the
    /// current frame.
    ///
    /// Panics if the bytes are not valid UTF-8 (a content error in the asset).
    pub fn text_string(&self) -> String {
        let pinned = self.pin();
        pinned
            .as_str()
            .expect("blyt: text resource is not valid UTF-8")
            .into()
    }
}

impl PinnedText {
    /// The content bytes with the build-appended trailing NUL stripped (#166).
    fn content(&self) -> &[u8] {
        let raw = self.raw();
        match raw.split_last() {
            Some((&0, rest)) => rest,
            // A text resource is always stored with a trailing NUL; anything
            // else means a non-text resource reached this guard, which the
            // typed API prevents. Be lenient rather than panic.
            _ => raw,
        }
    }

    /// Borrow the pinned content as `&str`, validating UTF-8. The trailing
    /// storage NUL is excluded.
    pub fn as_str(&self) -> Result<&str, Utf8Error> {
        core::str::from_utf8(self.content())
    }
}

impl LoadedBytes {
    /// Copy the resource's exact bytes into an owned `Vec<u8>` — no UTF-8
    /// validation and no NUL games, so it round-trips binary blobs faithfully.
    /// Unlike a [`pin`](LoadedBytes::pin), the copy outlives the current frame.
    pub fn bytes_vec(&self) -> Vec<u8> {
        let pinned = self.pin();
        pinned.as_bytes().into()
    }
}

impl PinnedBytes {
    /// Borrow the pinned bytes verbatim.
    pub fn as_bytes(&self) -> &[u8] {
        self.raw()
    }

    /// The raw pointer and byte length, for passing to a C API. The pointer is
    /// valid only while this guard is alive (the current frame).
    pub fn as_raw(&self) -> (*const c_void, usize) {
        (self.ptr, self.size)
    }
}
