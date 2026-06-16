// Stage 4 — probe cart for digest equivalence across hosts.
//
// Increments a local counter each frame and emits "frame=N value=M" via
// blyt_console_debug.  The host digest harness hashes the debug output
// stream with FNV-1a-64; the hash must be bit-identical on arm64 and amd64.
//
// Note (FV-7): The original design used blyt_buffer_get/set_u32 with
// hardcoded handles (kBuf=1, kField=0x00010001).  Those calls are no-ops
// because no state schema is registered — the buffer API requires a
// packer-generated .cart.config FlatBuffer section that maps buffer/field
// IDs to storage.  For the spike, local state is used instead.
// Classification: leaked assumption — the buffer API is schema-gated;
// a production Swift build would need `blyt build` packer integration to
// generate schema constants.  Expected integration point, not a design flaw.

var probeCounter: UInt32 = 0
var probeFrame: Int32 = 0

func emitFrameValue(_ frame: Int32, _ value: UInt32) {
    var msg: (CChar, CChar, CChar, CChar, CChar, CChar,     // "frame="
              CChar, CChar, CChar, CChar, CChar, CChar, CChar, CChar, CChar, CChar,  // frame digits
              CChar,                                         // " "
              CChar, CChar, CChar, CChar, CChar, CChar,     // "value="
              CChar, CChar, CChar, CChar, CChar, CChar, CChar, CChar, CChar, CChar,  // value digits
              CChar) =                                       // null terminator
        (102,114,97,109,101,61, 0,0,0,0,0,0,0,0,0,0, 32,
         118,97,108,117,101,61, 0,0,0,0,0,0,0,0,0,0, 0)

    var f = frame >= 0 ? UInt32(bitPattern: frame) : 0
    var pos = 15
    if f == 0 {
        withUnsafeMutableBytes(of: &msg) { ptr in ptr[pos] = 48 }
        pos -= 1
    } else {
        while f > 0 && pos >= 6 {
            withUnsafeMutableBytes(of: &msg) { ptr in ptr[pos] = UInt8(48 + (f % 10)) }
            f /= 10
            pos -= 1
        }
    }
    while pos >= 6 {
        withUnsafeMutableBytes(of: &msg) { ptr in ptr[pos] = 32 }
        pos -= 1
    }

    var v = value
    pos = 32
    if v == 0 {
        withUnsafeMutableBytes(of: &msg) { ptr in ptr[pos] = 48 }
        pos -= 1
    } else {
        while v > 0 && pos >= 23 {
            withUnsafeMutableBytes(of: &msg) { ptr in ptr[pos] = UInt8(48 + (v % 10)) }
            v /= 10
            pos -= 1
        }
    }
    while pos >= 23 {
        withUnsafeMutableBytes(of: &msg) { ptr in ptr[pos] = 32 }
        pos -= 1
    }

    withUnsafeBytes(of: &msg) { ptr in
        if let base = ptr.baseAddress {
            blyt_console_debug(base.assumingMemoryBound(to: CChar.self))
        }
    }
}

@_cdecl("blyt_cart_init") public func cartInit4() {
    probeCounter &+= 1
}

@_cdecl("blyt_cart_update") public func cartUpdate4() {
    probeCounter &+= 1
    emitFrameValue(probeFrame, probeCounter)
    probeFrame &+= 1
    if probeFrame >= 10 {
        blyt_quit()
    }
}

@_cdecl("blyt_cart_draw") public func cartDraw4() {}
