/* Save/load API — Rust wrappers (ADR-0087, ADR-0125) */

extern "C" {
    fn blyt_save_write(slot: u32) -> u32;
    fn blyt_save_read(slot: u32) -> u32;
}

/// Save current state buffers to save slot `slot`.
/// Returns 0 (BLYT_OK) on success.
pub fn save_write(slot: u32) -> u32 {
    unsafe { blyt_save_write(slot) }
}

/// Restore state buffers from save slot `slot`.
/// Returns 0 (BLYT_OK) on success.
pub fn save_read(slot: u32) -> u32 {
    unsafe { blyt_save_read(slot) }
}
