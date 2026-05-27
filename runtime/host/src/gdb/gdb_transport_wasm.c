/* GDB RSP WebSocket transport — WASM (Emscripten) frontend.
 *
 * Opens an outbound WebSocket to the blyt-run relay at
 * ws://127.0.0.1:<port>/gdb.  WebSocket frames carry full GDB RSP framing
 * ($payload#csum).  Acks (+/-) are handled transparently.
 *
 * Because WASM is single-threaded, recv_pkt is non-blocking: it returns -1
 * if no frame is queued.  The caller (wasm_loop) polls via fc_gdb_stub_poll()
 * once per animation tick while the cart is in BLYT_RUN_GDB_PAUSED state.
 *
 * on_stop is a no-op: the WASM run loop returns BLYT_RUN_GDB_PAUSED and
 * re-enters the event loop, which delivers WebSocket messages each tick.
 *
 * Compiled only for Emscripten builds with BLYT_GDB defined.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <emscripten.h>

#include "gdb_stub.h"

#define MAX_PKT 4096

/* Forward-declare internal gdb_stub helper. */
extern void fc_gdb_stub_set_has_client(int val);

/* ── EM_JS WebSocket helpers ──────────────────────────────────────────────── */

/* clang-format off */

EM_JS(void, wgdb_ws_open_js, (int port), {
    var url = 'ws://127.0.0.1:' + port + '/gdb';
    var ws  = new WebSocket(url);
    Module._wgdb_ws      = ws;
    Module._wgdb_queue   = [];
    Module._wgdb_connected = 0;
    ws.onopen    = function()  { Module._wgdb_connected = 1; };
    ws.onmessage = function(e) {
        var s = typeof e.data === 'string' ? e.data : e.data.toString();
        Module._wgdb_queue.push(s);
    };
    ws.onclose   = function()  { Module._wgdb_connected = 0; };
    ws.onerror   = function()  { Module._wgdb_connected = 0; };
});

EM_JS(void, wgdb_ws_send_js, (const char *msg_ptr), {
    if (!Module._wgdb_ws || Module._wgdb_ws.readyState !== 1) return;
    Module._wgdb_ws.send(UTF8ToString(msg_ptr));
});

EM_JS(int, wgdb_queue_length, (void), {
    return (Module._wgdb_queue && Module._wgdb_queue.length) ? Module._wgdb_queue.length : 0;
});

EM_JS(char *, wgdb_dequeue_frame, (void), {
    if (!Module._wgdb_queue || !Module._wgdb_queue.length) return 0;
    var s = Module._wgdb_queue.shift();
    var len = lengthBytesUTF8(s) + 1;
    var ptr = _malloc(len);
    stringToUTF8(s, ptr, len);
    return ptr;
});

EM_JS(int, wgdb_is_connected_js, (void), {
    return Module._wgdb_connected ? 1 : 0;
});

/* clang-format on */

/* ── GDB RSP framing ──────────────────────────────────────────────────────── */

static int wgdb_send_pkt(const char *payload) {
    size_t plen = strlen(payload);
    char *buf = malloc(plen + 8);
    if (!buf)
        return -1;
    int csum = 0;
    for (size_t i = 0; i < plen; i++)
        csum += (unsigned char)payload[i];
    snprintf(buf, plen + 8, "$%s#%02x", payload, csum & 0xff);
    wgdb_ws_send_js(buf);
    free(buf);
    return 0;
}

/* Extract GDB RSP payload from a full framed string "$payload#csum".
 * Returns payload length, or -1 if the frame is an ack/invalid. */
static int wgdb_extract_payload(const char *frame, char *buf, size_t cap) {
    /* Skip leading ack chars (+/-). */
    while (*frame == '+' || *frame == '-')
        frame++;
    if (*frame != '$')
        return -1;
    frame++; /* skip '$' */
    const char *end = strchr(frame, '#');
    if (!end)
        return -1;
    size_t plen = (size_t)(end - frame);
    if (plen + 1 > cap)
        return -1;
    memcpy(buf, frame, plen);
    buf[plen] = '\0';
    /* Send ack. */
    wgdb_ws_send_js("+");
    return (int)plen;
}

static int wgdb_recv_pkt(char *buf, size_t cap) {
    /* Drain ack-only frames and find a real packet. */
    while (wgdb_queue_length() > 0) {
        char *frame = wgdb_dequeue_frame();
        if (!frame)
            break;
        int n = wgdb_extract_payload(frame, buf, cap);
        free(frame);
        if (n >= 0)
            return n;
        /* n < 0: ack-only frame, continue draining. */
    }
    return -1;
}

static void wgdb_on_stop(void) {
    /* No-op: WASM returns BLYT_RUN_GDB_PAUSED, letting the event loop
     * deliver WebSocket messages each animation tick. */
}

static const fc_gdb_transport_t wasm_transport = {
    .send_pkt = wgdb_send_pkt,
    .recv_pkt = wgdb_recv_pkt,
    .on_stop = wgdb_on_stop,
};

/* ── Public API ───────────────────────────────────────────────────────────── */

void fc_gdb_transport_wasm_open(int relay_port) {
    fc_gdb_stub_set_transport(&wasm_transport);
    wgdb_ws_open_js(relay_port);
}

void fc_gdb_transport_wasm_shutdown(void) {
    EM_ASM({
        if (Module._wgdb_ws) {
            try {
                Module._wgdb_ws.close(1000, 'shutdown');
            }
            catch(e) {
            }
            Module._wgdb_ws = null;
        }
        Module._wgdb_connected = 0;
    });
    fc_gdb_stub_set_has_client(0);
}

int fc_gdb_transport_wasm_is_connected(void) {
    return wgdb_is_connected_js();
}
