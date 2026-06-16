// Stage 1 probe cart — minimal blyt cart in Embedded Swift.
//
// Provides blyt_cart_init/update/draw as @_cdecl exports so the linker can
// find them (mirroring the C/Rust pattern; entry-point retention via -Wl,-u).
//
// Calls blyt_console_debug to produce observable output, then quits after
// one frame (matching the hello-c pattern for headless smoke tests).

var frameCount: Int32 = 0

@_cdecl("blyt_cart_init") public func cartInit() {
    blyt_console_debug("swift-cart: init")
}

@_cdecl("blyt_cart_update") public func cartUpdate() {
    blyt_console_debug("swift-cart: update")
    frameCount &+= 1
    if frameCount >= 1 {
        blyt_quit()
    }
}

@_cdecl("blyt_cart_draw") public func cartDraw() {
    blyt_console_debug("swift-cart: draw")
}
