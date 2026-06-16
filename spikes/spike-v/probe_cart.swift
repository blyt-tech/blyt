// Stage 4 — probe cart for digest equivalence against C and Rust.
//
// Logic mirrors the Spike O reference cart: increment a u32 counter in the
// state buffer each frame (init + update), emit the frame index and counter
// value as a debug string. The host digest harness hashes the debug output
// stream with FNV-1a-64; the hash must match the equivalent C and Rust carts
// on both arm64 and amd64.
//
// State: one slot in a synthetic buffer (direct blyt_buffer_* calls with
// hardcoded buffer/field handles matching those used by the C reference cart).
// No packer output needed for the spike — constants are hardcoded per Finding O-2.

// Hardcoded buffer/field handles — must match probe_cart.c / probe_cart.rs.
// blyt_buffer_h: buffer 1 (arbitrary; 1-based per ADR-0009)
// blyt_field_h: field index 1 in buffer 1 → packed value 0x00010001
let kBuf: UInt32 = 1
let kField: UInt32 = 0x00010001
let kSlot: Int32 = 0

var probeFrame: Int32 = 0

// Emit "frame=N value=M\n" via blyt_console_debug.
// Avoids String (not available in Embedded without stdlib); uses a stack buffer.
func emitFrameValue(_ frame: Int32, _ value: UInt32) {
    // Format: "frame=NNNNNNNNN value=MMMMMMMM" (fits in 40 bytes + null)
    var buf: (CChar, CChar, CChar, CChar, CChar, CChar,  // "frame="
              CChar, CChar, CChar, CChar, CChar, CChar, CChar, CChar, CChar, CChar,  // frame digits
              CChar,  // " "
              CChar, CChar, CChar, CChar, CChar, CChar,  // "value="
              CChar, CChar, CChar, CChar, CChar, CChar, CChar, CChar, CChar, CChar,  // value digits
              CChar) =  // null terminator
        (102,114,97,109,101,61, 0,0,0,0,0,0,0,0,0,0, 32,
         118,97,108,117,101,61, 0,0,0,0,0,0,0,0,0,0, 0)

    // Write frame decimal into positions 6..15 (right-aligned, space-filled)
    var f = frame >= 0 ? UInt32(bitPattern: frame) : 0
    var pos = 15
    if f == 0 {
        withUnsafeMutableBytes(of: &buf) { ptr in ptr[pos] = 48 }
        pos -= 1
    } else {
        while f > 0 && pos >= 6 {
            withUnsafeMutableBytes(of: &buf) { ptr in ptr[pos] = UInt8(48 + (f % 10)) }
            f /= 10
            pos -= 1
        }
    }
    // Fill remaining frame-digit positions with spaces
    while pos >= 6 {
        withUnsafeMutableBytes(of: &buf) { ptr in ptr[pos] = 32 }
        pos -= 1
    }

    // Write value decimal into positions 23..32 (right-aligned, space-filled)
    var v = value
    pos = 32
    if v == 0 {
        withUnsafeMutableBytes(of: &buf) { ptr in ptr[pos] = 48 }
        pos -= 1
    } else {
        while v > 0 && pos >= 23 {
            withUnsafeMutableBytes(of: &buf) { ptr in ptr[pos] = UInt8(48 + (v % 10)) }
            v /= 10
            pos -= 1
        }
    }
    while pos >= 23 {
        withUnsafeMutableBytes(of: &buf) { ptr in ptr[pos] = 32 }
        pos -= 1
    }

    withUnsafeBytes(of: &buf) { ptr in
        if let base = ptr.baseAddress {
            blyt_console_debug(base.assumingMemoryBound(to: CChar.self))
        }
    }
}

@_cdecl("blyt_cart_init") public func cartInit4() {
    let v = blyt_buffer_get_u32(kBuf, kSlot, kField)
    blyt_buffer_set_u32(kBuf, kSlot, kField, v &+ 1)
}

@_cdecl("blyt_cart_update") public func cartUpdate4() {
    let v = blyt_buffer_get_u32(kBuf, kSlot, kField)
    blyt_buffer_set_u32(kBuf, kSlot, kField, v &+ 1)

    let newV = blyt_buffer_get_u32(kBuf, kSlot, kField)
    emitFrameValue(probeFrame, newV)
    probeFrame &+= 1

    if probeFrame >= 10 {
        blyt_quit()
    }
}

@_cdecl("blyt_cart_draw") public func cartDraw4() {}
