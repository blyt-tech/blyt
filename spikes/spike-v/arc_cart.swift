// Stage 3 — ARC + allocator probe.
//
// Uses a class to force ARC. Under Embedded Swift targeting RV32 with the A
// extension, ARC should lower to native AMO instructions (amoadd.w.aqrl or
// lr.w/sc.w), not libcalls. This mirrors Spike P's Arc<T> finding for Rust.
//
// Verification: llvm-objdump -d on the linked ELF should show amoadd or lr.w.
// Runtime: the cart must run without illegal-instruction traps or allocator panics.

final class Counter {
    var value: Int32 = 0

    func increment() {
        value &+= 1
    }

    func asString() -> Int32 {
        return value
    }
}

var counter: Counter? = nil
var frame3Count: Int32 = 0

@_cdecl("blyt_cart_init") public func cartInit3() {
    counter = Counter()
    blyt_console_debug("arc-cart: init")
}

@_cdecl("blyt_cart_update") public func cartUpdate3() {
    counter?.increment()
    frame3Count &+= 1
    if frame3Count >= 10 {
        blyt_quit()
    }
}

@_cdecl("blyt_cart_draw") public func cartDraw3() {
    if let c = counter {
        // Just access the value to ensure ARC retain/release paths exercise
        let _ = c.asString()
    }
}
