//! Named color-index constants (ADR-0059, issue #203).
//!
//! Plain `u8` palette *indices* (not tagged handles like [`crate::gfx`]'s
//! `PALETTE_*`), mirroring `blyt.h`'s `BLYT_EGA_*` / `BLYT_VGA_*` /
//! `BLYT_AURORA_*` / `BLYT_<NAME>`. The EGA-16 is the shared naming vocabulary;
//! each bundled palette ships its own set matching that palette's layout, so
//! the same name is a different index per palette.
//!
//! Use the module matching the active palette ([`ega`], [`vga`], [`aurora`]);
//! the crate-level re-export gives the zero-config default aliases
//! (`blyt::color::WHITE` == the console default palette's white). Custom (cart)
//! palettes get packer-generated `C_*` constants instead.
//!
//! EGA quirk: index 6 is `BROWN` (`#AA5500`), not a dark yellow, and there is
//! no bright-brown — the bright row is the `BR_*` names.

/// Canonical EGA-16 indices 0-15.
pub mod ega {
    pub const BLACK: u8 = 0;
    pub const BLUE: u8 = 1;
    pub const GREEN: u8 = 2;
    pub const CYAN: u8 = 3;
    pub const RED: u8 = 4;
    pub const MAGENTA: u8 = 5;
    pub const BROWN: u8 = 6;
    pub const LTGRAY: u8 = 7;
    pub const DKGRAY: u8 = 8;
    pub const BR_BLUE: u8 = 9;
    pub const BR_GREEN: u8 = 10;
    pub const BR_CYAN: u8 = 11;
    pub const BR_RED: u8 = 12;
    pub const BR_MAGENTA: u8 = 13;
    pub const BR_YELLOW: u8 = 14;
    pub const WHITE: u8 = 15;
}

/// VGA's low 16 are the EGA 16 (identical RGB) — same indices.
pub mod vga {
    pub use super::ega::*;
}

/// Aurora nearest-to-EGA indices (issue #203 distance table): grays and
/// black/white land exact, chromatics are hue-shifted approximations.
pub mod aurora {
    pub const BLACK: u8 = 0;
    pub const BLUE: u8 = 223;
    pub const GREEN: u8 = 185;
    pub const CYAN: u8 = 195;
    pub const RED: u8 = 155;
    pub const MAGENTA: u8 = 239;
    pub const BROWN: u8 = 165;
    pub const LTGRAY: u8 = 10;
    pub const DKGRAY: u8 = 5;
    pub const BR_BLUE: u8 = 219;
    pub const BR_GREEN: u8 = 189;
    pub const BR_CYAN: u8 = 201;
    pub const BR_RED: u8 = 160;
    pub const BR_MAGENTA: u8 = 236;
    pub const BR_YELLOW: u8 = 175;
    pub const WHITE: u8 = 15;
}

/// Unprefixed default aliases → the console-default palette (aurora).
pub use aurora::*;
