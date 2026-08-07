/* runtime/host/src/dap/dap_transport_wasm.c
 *
 * DAP transport for the WASM frontend — the WebSocket + Lua-coroutine-yield half
 * of the host-side Lua debugger.  The protocol + Lua-inspection logic lives in
 * dap_lua_inspect.c (shared with the native TCP transport, issue #234); this TU
 * supplies only the WASM-specific I/O (outbound WebSocket to the blyt-run relay)
 * and the WASM-specific pause primitive (yield the Lua coroutine back to the
 * event loop instead of blocking a thread).
 *
 * The WASM binary connects outbound WebSocket to the blyt-run relay server at
 * ws://127.0.0.1:<relay_port>/dap.  The relay bridges to the external DAP client
 * (VS Code extension or dap_test.mjs test driver).  Protocol: raw JSON strings
 * over WebSocket (no Content-Length framing; WebSocket frames are already
 * self-delimiting).
 *
 * Pausing uses the Lua coroutine yield/resume pattern: fc_dap_pause_loop calls
 * lua_yield(L, 0), which unwinds back to the lua_resume in wasm_lua_loop without
 * blocking — the browser/Node.js event loop keeps delivering WebSocket messages.
 * fc_dap_poll_messages drains the queue each tick; on continue, fc_dap_do_resume
 * applies the step mode before the next lua_resume.  The lua_State* is a real
 * host pointer (Lua runs natively in WASM), enabling full inspection.
 *
 * Compiled only for Emscripten builds (BLYT_DAP + __EMSCRIPTEN__).
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <emscripten.h>

#include "lua.h"

#include "blyt_trace.h"
#include "dap_lua_inspect.h"
#include "dap_server.h"

/* ── WASM-local transport state ────────────────────────────────────────────── */

/* Set by fc_dap_pause_loop, test-and-cleared by fc_dap_hook_yielded, so
 * wasm_lua_loop can tell a DAP-pause yield from the normal frame-boundary
 * coroutine.yield(). */
static int g_wdap_hook_yielded;

/* ── WebSocket JS helpers ───────────────────────────────────────────────────── */

/* clang-format off */

/* Open WebSocket to relay and install onopen/onmessage/onclose handlers. */
EM_JS(void, wdap_ws_open_js, (int port), {
    var url = 'ws://127.0.0.1:' + port + '/dap';
    var ws  = new WebSocket(url);
    Module._wdap_ws      = ws;
    Module._wdap_queue   = [];
    ws.onopen    = function()  { Module._wdap_connected = 1; };
    ws.onmessage = function(e) { Module._wdap_queue.push(typeof e.data === 'string' ? e.data : e.data.toString()); };
    ws.onclose   = function()  { Module._wdap_connected = 0; };
    ws.onerror   = function()  { Module._wdap_connected = 0; };
});

EM_JS(int, wdap_is_connected_js, (void), {
    return Module._wdap_connected ? 1 : 0;
});

EM_JS(void, wdap_ws_send_js, (const char *json_ptr), {
    if (!Module._wdap_ws || Module._wdap_ws.readyState !== 1) return;
    Module._wdap_ws.send(UTF8ToString(json_ptr));
});

EM_JS(int, wdap_queue_length, (void), {
    return (Module._wdap_queue && Module._wdap_queue.length) ? Module._wdap_queue.length : 0;
});

EM_JS(char *, wdap_dequeue_json, (void), {
    if (!Module._wdap_queue || !Module._wdap_queue.length) return 0;
    var s = Module._wdap_queue.shift();
    var len = lengthBytesUTF8(s) + 1;
    var ptr = _malloc(len);
    stringToUTF8(s, ptr, len);
    return ptr;
});

/* clang-format on */

/* Trace WebSocket connect/disconnect transitions.  There is no connection
 * callback into C, so the transition is detected where state is polled. */
static void wdap_trace_conn(void) {
    static int was_connected;
    int now = wdap_is_connected_js();
    if (now != was_connected) {
        blyt_tracef(BLYT_TRACE_DAP, now ? "client connected" : "client disconnected");
        was_connected = now;
    }
}

/* ── Transport ops for the shared core ─────────────────────────────────────── */

static void wdap_send(const char *json) {
    blyt_tracef(BLYT_TRACE_DAP, "send %s", json);
    wdap_ws_send_js(json);
}

static int wdap_is_connected(void) {
    return wdap_is_connected_js();
}

static const dap_lua_transport_ops_t wdap_ops = {
    .send = wdap_send,
    .is_connected = wdap_is_connected,
};

/* Drain the message queue and dispatch each JSON message. */
static void drain_queue(void) {
    wdap_trace_conn();
    while (wdap_queue_length() > 0) {
        char *json = wdap_dequeue_json();
        if (!json)
            break;
        blyt_tracef(BLYT_TRACE_DAP, "recv %s", json);
        dap_lua_dispatch(json);
        free(json);
    }
}

/* ── Public DAP interface (dap_server.h) ───────────────────────────────────── */

void fc_dap_poll_messages(void) {
    drain_queue();
}

int fc_consolelua_dap_listen(int relay_port) {
    dap_lua_set_transport(&wdap_ops);
    dap_lua_reset();
    wdap_ws_open_js(relay_port);
    return relay_port;
}

void fc_consolelua_dap_shutdown(void) {
    EM_ASM({
        if (Module._wdap_ws) {
            try {
                Module._wdap_ws.close(1000, 'shutdown');
            }
            catch(e) {
            }
            Module._wdap_ws = null;
        }
        Module._wdap_connected = 0;
    });
}

int fc_dap_configuration_done(void) {
    drain_queue();
    return dap_lua_configuration_done();
}

/* ── Master-hook callbacks (WASM direct-Lua path) ──────────────────────────── */

bool fc_dap_should_break(lua_State *L, lua_Debug *ar) {
    return dap_lua_should_break(L, ar);
}

void fc_dap_pause_loop(lua_State *L, lua_Debug *ar) {
    (void)ar;
    dap_lua_enter_paused(L);
    g_wdap_hook_yielded = 1; /* signal to wasm_lua_loop */

    /* Yield the Lua coroutine.  In the line-hook context lua_yield() returns 0
     * (does NOT throw): luaG_traceexec will set CIST_HOOKYIELD and throw
     * LUA_YIELD after this function returns, correctly preserving the VM PC so
     * execution resumes at the right instruction after fc_dap_do_resume(). */
    lua_yield(L, 0);
}

int fc_dap_hook_yielded(void) {
    if (g_wdap_hook_yielded) {
        g_wdap_hook_yielded = 0;
        return 1;
    }
    return 0;
}

int fc_dap_continue_pending(void) {
    return dap_lua_continue_pending();
}

void fc_dap_do_resume(void) {
    dap_lua_apply_resume();
}

void fc_dap_output(const char *msg) {
    dap_lua_output(msg);
}

void fc_dap_emit_loaded_source(const char *source_path) {
    dap_lua_emit_loaded_source(source_path);
}

int fc_dap_is_restart_pending(void) {
    return dap_lua_restart_pending();
}

int fc_dap_get_condition(char *buf, size_t n) {
    /* WASM: conditions are evaluated directly in dap_lua_should_break; not used. */
    if (n > 0)
        buf[0] = '\0';
    return 0;
}

int fc_dap_on_condition_result(int result) {
    (void)result;
    return 0;
}

int fc_dap_exception_filter(void) {
    return dap_lua_exception_filter();
}

int fc_dap_on_exception(const char *msg, int is_uncaught) {
    int paused = dap_lua_on_exception(msg, is_uncaught);
    if (paused)
        g_wdap_hook_yielded = 1; /* wasm_lua_loop treats the pause as a DAP stop */
    return paused;
}

int blyt_dap_report_exception(lua_State *L, int is_uncaught) {
    const char *msg = lua_tostring(L, -1);
    if (!msg)
        msg = "(error)";
    return fc_dap_on_exception(msg, is_uncaught);
}

void blyt_dap_capture_exception(lua_State *L) {
    /* Single-threaded here — just publish the snapshot the message handler walked
     * (#319); dap_lua_capture_exception no-ops without a client + exception filter. */
    dap_lua_capture_exception(L);
}

/* ── ECALL stubs (RV32 ELF cart path — not used in WASM Lua builds) ───────── */

int fc_dap_check_hook_line(const char *source, int line, int depth) {
    (void)source;
    (void)line;
    (void)depth;
    return 0;
}

void fc_dap_host_send(const char *json, size_t len) {
    (void)json;
    (void)len;
}

int fc_dap_host_recv(char *buf, size_t max_len) {
    (void)buf;
    (void)max_len;
    return 0;
}
