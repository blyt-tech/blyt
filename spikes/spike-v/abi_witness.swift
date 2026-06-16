// Stage 2 — ilp32d ABI witness.
//
// A @_cdecl function takes a Double and emits its raw IEEE-754 bits as hex.
// Under ilp32d hard-float ABI, the Double argument arrives in fa0; the raw
// bits are 0x3fe0000000000000 for 0.5.
//
// Note: `fmv.x.d` does not exist on RV32 (it is a 64-bit instruction that
// moves an f64 to *two* integer registers: rd=bits[31:0], rd+1=bits[63:32]).
// Instead we read the bits via a pointer cast through a local buffer, which
// the compiler lowers to fsd + lw/lw on RV32, correctly extracting the bits.
//
// The host-side test invokes: set_param(0, 0.5) → function reads fa0 bits.
// Expected output: param=3fe0000000000000

// Raw hex helper: emits 16-char lowercase hex from a UInt64.
// Uses blyt_console_debug via the C bridge.
func emitHex(_ value: UInt64) {
    var buf: (UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8,
              UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8) =
        (0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)
    let digits: [UInt8] = [48,49,50,51,52,53,54,55,56,57,97,98,99,100,101,102]
    // "param=" prefix: 112,97,114,97,109,61
    var msg: (UInt8, UInt8, UInt8, UInt8, UInt8, UInt8,
              UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8,
              UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8) =
        (112,97,114,97,109,61,
         0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0)
    for i in 0..<16 {
        let nibble = Int((value >> (60 - i * 4)) & 0xF)
        withUnsafeMutableBytes(of: &msg) { ptr in
            ptr[6 + i] = digits[nibble]
        }
    }
    withUnsafeBytes(of: &msg) { ptr in
        if let base = ptr.baseAddress {
            blyt_console_debug(base.assumingMemoryBound(to: CChar.self))
        }
    }
}

@_cdecl("blyt_abi_witness_double") public func abiWitnessDouble(_ d: Double) {
    // Read raw bits via pointer cast.
    var val = d
    let bits = withUnsafeBytes(of: &val) { ptr -> UInt64 in
        let lo = UInt64(ptr.load(fromByteOffset: 0, as: UInt32.self))
        let hi = UInt64(ptr.load(fromByteOffset: 4, as: UInt32.self))
        return lo | (hi << 32)
    }
    emitHex(bits)
}

var frame2Count: Int32 = 0

@_cdecl("blyt_cart_init") public func cartInit2() {
    blyt_console_debug("abi-witness: init")
}

@_cdecl("blyt_cart_update") public func cartUpdate2() {
    frame2Count &+= 1
    if frame2Count == 1 {
        abiWitnessDouble(0.5)
    }
    if frame2Count >= 2 {
        blyt_quit()
    }
}

@_cdecl("blyt_cart_draw") public func cartDraw2() {}
