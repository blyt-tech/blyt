//! Surfaces — the paletted 2D drawing API (ADR-0052/0086/0008, #195/#205).
//!
//! A [`Surface`] is a runtime-managed 256-colour paletted buffer. [`SCREEN`] is
//! the built-in 320x240 screen; off-screen surfaces are made with
//! [`Surface::create`] (draw-scoped: valid only for the current `draw()`, and
//! counted against the 16 MB memory budget). Two ways to draw:
//!
//! - **Tier-1 serviced ops** ([`Surface::clear`], [`rect_fill`](Surface::rect_fill),
//!   [`line`](Surface::line), [`pixel`](Surface::pixel), [`blit`](Surface::blit)) —
//!   one call each, rasterized by the runtime.
//! - **Tier-2 lock** ([`Surface::acquire`]) — a [`SurfaceLock`] giving raw
//!   per-pixel access ([`pixels_mut`](SurfaceLock::pixels_mut)) plus the same
//!   primitives drawn in-process. The lock is released automatically on drop
//!   (flushing the buffer back); it is `!Send` and borrows the surface, so it
//!   cannot escape the frame or the thread.

use core::marker::PhantomData;

extern "C" {
    fn blyt_surface_create(w: i32, h: i32) -> u32;
    fn blyt_surface_destroy(surface: u32);
    fn blyt_surface_clear(dst: u32, color: u8);
    fn blyt_surface_pixel(dst: u32, x: i32, y: i32, color: u8);
    fn blyt_surface_rect_fill(dst: u32, x: i32, y: i32, w: i32, h: i32, color: u8);
    fn blyt_surface_line(dst: u32, x0: i32, y0: i32, x1: i32, y1: i32, color: u8);
    fn blyt_surface_blit(dst: u32, src: u32, x: i32, y: i32);
    fn blyt_surface_acquire(surface: u32, out: *mut RawLock) -> i32;
    fn blyt_surface_release(lock: *mut RawLock);
    fn blyt_gfx_palette_set(palette: u32);

    // In-lock primitives (the freestanding rasterizer, exported by libblyt32.so).
    fn blyt_raster_clear(fb: *mut u8, stride: i32, width: i32, height: i32, color: u8);
    fn blyt_raster_pixel(fb: *mut u8, stride: i32, width: i32, height: i32, x: i32, y: i32, c: u8);
    fn blyt_raster_rect_fill(
        fb: *mut u8,
        stride: i32,
        width: i32,
        height: i32,
        x: i32,
        y: i32,
        w: i32,
        h: i32,
        c: u8,
    );
    fn blyt_raster_line(
        fb: *mut u8,
        stride: i32,
        width: i32,
        height: i32,
        x0: i32,
        y0: i32,
        x1: i32,
        y1: i32,
        c: u8,
    );
}

/// A handle to a runtime-managed paletted surface (a console-wide tagged u32).
#[derive(Clone, Copy, PartialEq, Eq)]
pub struct Surface(pub u32);

/// The built-in screen surface (SURFACE kind, slot 0) — never created or
/// destroyed. Mirrors `BLYT_SCREEN` in `blyt.h` / `blyt_handle.h`.
pub const SCREEN: Surface = Surface(0x4000_0000);

/// A palette handle (#201/#214): a built-in [`PALETTE_AURORA`] etc., or a
/// packer-emitted `R_<NAME>` for a cart-authored `.hex`/`.gpl`/`.pal` palette
/// file. Passed to [`palette_set`]. A console-wide tagged u32 (ADR-0134);
/// mirrors `blyt_palette_t` in `blyt.h`.
#[derive(Clone, Copy, PartialEq, Eq)]
pub struct Palette(pub u32);

impl Palette {
    /// Wrap a baked palette handle. Used by the packer-generated `R_<NAME>`
    /// constants (`blyt::Palette::new(0x…)`); `const` so they are usable in
    /// const context.
    pub const fn new(handle: u32) -> Palette {
        Palette(handle)
    }
}

/// Console-wide tagged handles of the four runtime-bundled built-in palettes
/// (issue #201, ADR-0042). Mirror `BLYT_PALETTE_*` in `blyt.h`.
pub const PALETTE_AURORA: Palette = Palette(0x2100_0001);
pub const PALETTE_VGA: Palette = Palette(0x2100_0002);
pub const PALETTE_EGA: Palette = Palette(0x2100_0003);
pub const PALETTE_CGA: Palette = Palette(0x2100_0004);
/// The console default — what an undeclared-palette cart auto-loads.
pub const PALETTE_DEFAULT: Palette = PALETTE_AURORA;

/// Load a palette wholesale (all 256 entries): a built-in [`PALETTE_AURORA`]
/// etc., or a cart palette asset `R_<NAME>`. A no-op on a handle that does not
/// resolve to a 256-entry palette (issue #201/#214).
pub fn palette_set(palette: Palette) {
    // SAFETY: pure scalar ECALL.
    unsafe { blyt_gfx_palette_set(palette.0) }
}

impl Surface {
    /// Create a blank `w`x`h` off-screen surface, draw-scoped to the current
    /// frame. Returns `None` if the size is invalid or it would exceed the
    /// 16 MB memory budget.
    pub fn create(w: i32, h: i32) -> Option<Surface> {
        // SAFETY: pure scalar ECALL.
        let h = unsafe { blyt_surface_create(w, h) };
        if h == 0 {
            None
        } else {
            Some(Surface(h))
        }
    }

    /// Free this surface early (surfaces are otherwise reaped at end of frame).
    /// A no-op on [`SCREEN`] or a stale handle.
    pub fn destroy(self) {
        // SAFETY: the runtime validates the handle.
        unsafe { blyt_surface_destroy(self.0) }
    }

    /// Fill the whole surface with one palette index.
    pub fn clear(self, color: u8) {
        // SAFETY: the runtime validates the handle and clips.
        unsafe { blyt_surface_clear(self.0, color) }
    }

    /// Set one pixel (off-surface coordinates are clipped).
    pub fn pixel(self, x: i32, y: i32, color: u8) {
        // SAFETY: the runtime validates the handle and clips.
        unsafe { blyt_surface_pixel(self.0, x, y, color) }
    }

    /// Fill a `w`x`h` rectangle at `(x,y)` (top/left inclusive), clipped.
    pub fn rect_fill(self, x: i32, y: i32, w: i32, h: i32, color: u8) {
        // SAFETY: the runtime validates the handle and clips.
        unsafe { blyt_surface_rect_fill(self.0, x, y, w, h, color) }
    }

    /// Draw a line between `(x0,y0)` and `(x1,y1)` inclusive.
    pub fn line(self, x0: i32, y0: i32, x1: i32, y1: i32, color: u8) {
        // SAFETY: the runtime validates the handle and clips.
        unsafe { blyt_surface_line(self.0, x0, y0, x1, y1, color) }
    }

    /// Copy the whole `src` surface into this one at `(x,y)`, clipped.
    pub fn blit(self, src: Surface, x: i32, y: i32) {
        // SAFETY: the runtime validates both handles and clips.
        unsafe { blyt_surface_blit(self.0, src.0, x, y) }
    }

    /// Acquire an exclusive per-pixel lock. Returns `None` if the surface is
    /// unresolvable or already locked. The lock releases on drop.
    pub fn acquire(&self) -> Option<SurfaceLock<'_>> {
        let mut raw = RawLock::zeroed();
        // SAFETY: `raw` is a valid, writable out-param of the expected layout.
        let ok = unsafe { blyt_surface_acquire(self.0, &mut raw) };
        if ok != 0 && !raw.pixels.is_null() {
            Some(SurfaceLock {
                raw,
                _marker: PhantomData,
            })
        } else {
            None
        }
    }
}

/// The lock struct the runtime fills — must match `blyt_lock_t` in `blyt.h`.
#[repr(C)]
struct RawLock {
    pixels: *mut u8,
    stride: i32,
    w: i32,
    h: i32,
    token: u32,
}

impl RawLock {
    fn zeroed() -> Self {
        RawLock {
            pixels: core::ptr::null_mut(),
            stride: 0,
            w: 0,
            h: 0,
            token: 0,
        }
    }
}

/// An exclusive per-pixel lock on a surface (tier-2). Gives raw access to the
/// materialized buffer and in-lock primitives; the buffer is flushed back to the
/// surface when the lock is dropped. Not `Send`/`Sync` (the `PhantomData` holds a
/// raw pointer), and it borrows the surface, so it cannot outlive the frame.
pub struct SurfaceLock<'a> {
    raw: RawLock,
    _marker: PhantomData<(*mut u8, &'a Surface)>,
}

impl SurfaceLock<'_> {
    /// Surface width in pixels.
    pub fn width(&self) -> i32 {
        self.raw.w
    }

    /// Surface height in pixels.
    pub fn height(&self) -> i32 {
        self.raw.h
    }

    /// Bytes per row.
    pub fn stride(&self) -> i32 {
        self.raw.stride
    }

    /// The materialized buffer as a mutable slice of palette indices, row-major
    /// with `stride()` bytes per row.
    pub fn pixels_mut(&mut self) -> &mut [u8] {
        let len = (self.raw.stride as usize) * (self.raw.h as usize);
        // SAFETY: the runtime materialized a buffer of stride*h bytes at
        // raw.pixels, valid for the lifetime of the lock.
        unsafe { core::slice::from_raw_parts_mut(self.raw.pixels, len) }
    }

    /// Fill the whole locked surface with one palette index (in-process).
    pub fn clear(&mut self, color: u8) {
        // SAFETY: raw.pixels is a valid stride*h buffer; the rasterizer clips.
        unsafe { blyt_raster_clear(self.raw.pixels, self.raw.stride, self.raw.w, self.raw.h, color) }
    }

    /// Set one pixel (clipped).
    pub fn pixel(&mut self, x: i32, y: i32, color: u8) {
        // SAFETY: as above.
        unsafe {
            blyt_raster_pixel(self.raw.pixels, self.raw.stride, self.raw.w, self.raw.h, x, y, color)
        }
    }

    /// Fill a rectangle (clipped).
    pub fn rect_fill(&mut self, x: i32, y: i32, w: i32, h: i32, color: u8) {
        // SAFETY: as above.
        unsafe {
            blyt_raster_rect_fill(
                self.raw.pixels,
                self.raw.stride,
                self.raw.w,
                self.raw.h,
                x,
                y,
                w,
                h,
                color,
            )
        }
    }

    /// Draw a line (clipped).
    pub fn line(&mut self, x0: i32, y0: i32, x1: i32, y1: i32, color: u8) {
        // SAFETY: as above.
        unsafe {
            blyt_raster_line(
                self.raw.pixels,
                self.raw.stride,
                self.raw.w,
                self.raw.h,
                x0,
                y0,
                x1,
                y1,
                color,
            )
        }
    }
}

impl Drop for SurfaceLock<'_> {
    fn drop(&mut self) {
        // SAFETY: releasing flushes the buffer and invalidates the token.
        unsafe { blyt_surface_release(&mut self.raw) }
    }
}
