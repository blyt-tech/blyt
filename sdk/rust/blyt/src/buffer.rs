/* State buffer API — Rust wrappers (ADR-0009, ADR-0010, ADR-0057, ADR-0058) */

use core::ffi::c_void;

pub type BlytBufferH = u32;
pub type BlytFieldH = u32;
pub const BLYT_FIELD_NONE: BlytFieldH = 0;
pub const BLYT_INVALID_SLOT: i32 = -1;

extern "C" {
    fn blyt_buffer_get_f32(buf: BlytBufferH, slot: i32, field: BlytFieldH) -> f32;
    fn blyt_buffer_set_f32(buf: BlytBufferH, slot: i32, field: BlytFieldH, v: f32);
    fn blyt_buffer_get_i32(buf: BlytBufferH, slot: i32, field: BlytFieldH) -> i32;
    fn blyt_buffer_set_i32(buf: BlytBufferH, slot: i32, field: BlytFieldH, v: i32);
    fn blyt_buffer_get_u32(buf: BlytBufferH, slot: i32, field: BlytFieldH) -> u32;
    fn blyt_buffer_set_u32(buf: BlytBufferH, slot: i32, field: BlytFieldH, v: u32);
    fn blyt_buffer_get_i16(buf: BlytBufferH, slot: i32, field: BlytFieldH) -> i16;
    fn blyt_buffer_set_i16(buf: BlytBufferH, slot: i32, field: BlytFieldH, v: i16);
    fn blyt_buffer_get_u16(buf: BlytBufferH, slot: i32, field: BlytFieldH) -> u16;
    fn blyt_buffer_set_u16(buf: BlytBufferH, slot: i32, field: BlytFieldH, v: u16);
    fn blyt_buffer_get_i8(buf: BlytBufferH, slot: i32, field: BlytFieldH) -> i8;
    fn blyt_buffer_set_i8(buf: BlytBufferH, slot: i32, field: BlytFieldH, v: i8);
    fn blyt_buffer_get_u8(buf: BlytBufferH, slot: i32, field: BlytFieldH) -> u8;
    fn blyt_buffer_set_u8(buf: BlytBufferH, slot: i32, field: BlytFieldH, v: u8);
    fn blyt_buffer_get_bool(buf: BlytBufferH, slot: i32, field: BlytFieldH) -> bool;
    fn blyt_buffer_set_bool(buf: BlytBufferH, slot: i32, field: BlytFieldH, v: bool);
    fn blyt_buffer_alloc_slot(buf: BlytBufferH, out_slot: *mut i32) -> u32;
    fn blyt_buffer_free_slot(buf: BlytBufferH, slot: i32) -> u32;
}

pub fn get_f32(buf: BlytBufferH, slot: i32, field: BlytFieldH) -> f32 {
    unsafe { blyt_buffer_get_f32(buf, slot, field) }
}
pub fn set_f32(buf: BlytBufferH, slot: i32, field: BlytFieldH, v: f32) {
    unsafe { blyt_buffer_set_f32(buf, slot, field, v) }
}
pub fn get_i32(buf: BlytBufferH, slot: i32, field: BlytFieldH) -> i32 {
    unsafe { blyt_buffer_get_i32(buf, slot, field) }
}
pub fn set_i32(buf: BlytBufferH, slot: i32, field: BlytFieldH, v: i32) {
    unsafe { blyt_buffer_set_i32(buf, slot, field, v) }
}
pub fn get_u32(buf: BlytBufferH, slot: i32, field: BlytFieldH) -> u32 {
    unsafe { blyt_buffer_get_u32(buf, slot, field) }
}
pub fn set_u32(buf: BlytBufferH, slot: i32, field: BlytFieldH, v: u32) {
    unsafe { blyt_buffer_set_u32(buf, slot, field, v) }
}
pub fn get_i16(buf: BlytBufferH, slot: i32, field: BlytFieldH) -> i16 {
    unsafe { blyt_buffer_get_i16(buf, slot, field) }
}
pub fn set_i16(buf: BlytBufferH, slot: i32, field: BlytFieldH, v: i16) {
    unsafe { blyt_buffer_set_i16(buf, slot, field, v) }
}
pub fn get_u16(buf: BlytBufferH, slot: i32, field: BlytFieldH) -> u16 {
    unsafe { blyt_buffer_get_u16(buf, slot, field) }
}
pub fn set_u16(buf: BlytBufferH, slot: i32, field: BlytFieldH, v: u16) {
    unsafe { blyt_buffer_set_u16(buf, slot, field, v) }
}
pub fn get_i8(buf: BlytBufferH, slot: i32, field: BlytFieldH) -> i8 {
    unsafe { blyt_buffer_get_i8(buf, slot, field) }
}
pub fn set_i8(buf: BlytBufferH, slot: i32, field: BlytFieldH, v: i8) {
    unsafe { blyt_buffer_set_i8(buf, slot, field, v) }
}
pub fn get_u8(buf: BlytBufferH, slot: i32, field: BlytFieldH) -> u8 {
    unsafe { blyt_buffer_get_u8(buf, slot, field) }
}
pub fn set_u8(buf: BlytBufferH, slot: i32, field: BlytFieldH, v: u8) {
    unsafe { blyt_buffer_set_u8(buf, slot, field, v) }
}
pub fn get_bool(buf: BlytBufferH, slot: i32, field: BlytFieldH) -> bool {
    unsafe { blyt_buffer_get_bool(buf, slot, field) }
}
pub fn set_bool(buf: BlytBufferH, slot: i32, field: BlytFieldH, v: bool) {
    unsafe { blyt_buffer_set_bool(buf, slot, field, v) }
}

/// Allocate a slot in the buffer. Returns the slot index, or BLYT_INVALID_SLOT (-1).
pub fn alloc_slot(buf: BlytBufferH) -> i32 {
    let mut slot: i32 = BLYT_INVALID_SLOT;
    unsafe { blyt_buffer_alloc_slot(buf, &mut slot) };
    slot
}

pub fn free_slot(buf: BlytBufferH, slot: i32) {
    unsafe { blyt_buffer_free_slot(buf, slot) };
}

/* Suppress unused import warning for c_void in cfg(feature="...") paths */
#[allow(unused)]
const _: () = {
    let _ = core::mem::size_of::<*mut c_void>();
};
