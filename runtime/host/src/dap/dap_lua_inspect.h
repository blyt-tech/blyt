/* runtime/host/src/dap/dap_lua_inspect.h
 *
 * Transport-neutral core of the host-side Lua DAP debugger (issue #234).
 *
 * The DAP protocol + Lua-source-level inspection logic — breakpoint table,
 * sourceMap, step state, and the stateless request→response handlers
 * (stackTrace / scopes / variables / evaluate / setBreakpoints / …) — shared by
 * two transports that both run Lua natively against a real lua_State*:
 *   - dap_transport_wasm.c   (Emscripten WebSocket; pauses by yielding the Lua
 *                             coroutine back to the browser/Node event loop)
 *   - dap_transport_tcp_lua.c (native TCP + pthread; pauses by blocking the
 *                             execution thread — the native host-Lua path, #234)
 *
 * This TU exports ONLY the `dap_lua_*` namespace; every public DAP entry point
 * (fc_dap_* on WASM, fc_hostlua_dap_* on native) is a thin wrapper in the
 * transport.  That keeps it collision-free in the native blytdebug link, which
 * also pulls in dap_server.c (the emulated RV32 ECALL path, which owns the
 * fc_dap_* / fc_consolelua_dap_* symbols).
 *
 * NOT used by dap_server.c: the emulated path forwards inspection to the guest
 * VM over ECALLs and never touches a host lua_State, so it needs none of this.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Sizing — shared by the core and both transports. */
#define DAP_LUA_MAX_BREAKPOINTS 256
#define DAP_LUA_MAX_CONDITION 256
#define DAP_LUA_MAX_FRAMES 64
#define DAP_LUA_MAX_VARS 64
#define DAP_LUA_MAX_SOURCE_PATH 1024
#define DAP_LUA_MAX_SOURCES 64
#define DAP_LUA_MAX_SRCMAP 16
#define DAP_LUA_MAX_MSG (64 * 1024)

/* Transport hooks the core calls out through.  The core builds a complete JSON
 * message string and hands it to send(); is_connected() gates event emission so
 * the core never pushes to a dead link. */
typedef struct {
    void (*send)(const char *json); /* deliver one framed/complete DAP JSON message */
    int (*is_connected)(void); /* non-zero while a client is attached */
} dap_lua_transport_ops_t;

/* Install the transport hooks (call once, before any dispatch). */
void dap_lua_set_transport(const dap_lua_transport_ops_t *ops);

/* Reset all protocol state to a fresh session (breakpoints, sourceMap, step
 * mode, paused state, sources).  Called by a transport on (re)listen. */
void dap_lua_reset(void);

/* Parse + handle one inbound DAP request JSON, emitting the response/events via
 * the transport's send().  This is the whole client→server protocol surface. */
void dap_lua_dispatch(const char *json);

/* Master-hook callback body (transport-neutral): true if execution should stop
 * at the current line — an async pause is pending, or a breakpoint at this
 * source:line matches (its condition, if any, evaluated in-frame).  Records the
 * hit breakpoint id for the next stopped event. */
bool dap_lua_should_break(lua_State *L, lua_Debug *ar);

/* Enter the paused state for `L` and emit the DAP "stopped" event (reason
 * derived from pending pause / step mode / breakpoint).  The transport calls
 * this from its pause primitive, then either yields (WASM) or blocks and
 * services requests (native) until dap_lua_continue_pending() is set. */
void dap_lua_enter_paused(lua_State *L);

/* True once the client has sent continue / next / stepIn / stepOut / disconnect
 * — i.e. the pause primitive should return. */
int dap_lua_continue_pending(void);

/* Apply the pending step mode into fc_master_hook_cfg, clear the paused state,
 * and emit the "continued" event.  Call just before resuming execution. */
void dap_lua_apply_resume(void);

/* Non-zero once the client has sent configurationDone. */
int dap_lua_configuration_done(void);

/* Test-and-clear: non-zero if the client requested a restart. */
int dap_lua_restart_pending(void);

/* Current exception filter: 0=none, 1=uncaught, 2=all. */
int dap_lua_exception_filter(void);

/* Report a caught/uncaught Lua error; emits "stopped" (reason=exception) when
 * the filter matches and a client is attached.  Returns 1 if it paused. */
int dap_lua_on_exception(const char *msg, int is_uncaught);

/* Emit a DAP "output" event (routes to the Debug Console). */
void dap_lua_output(const char *msg);

/* Dedupe + emit a "loadedSource" event the first time a source is seen. */
void dap_lua_emit_loaded_source(const char *source_path);

/* The lua_State the debugger is currently paused on (NULL when running).  Note:
 * NULL does NOT imply "running" — an exception stop (dap_lua_on_exception) parks
 * with no live frame (paused_L == NULL) yet is still paused; use dap_lua_is_paused
 * to gate on the paused state itself. */
lua_State *dap_lua_paused_state(void);

/* Non-zero while the debugger is stopped (breakpoint/step OR an exception stop),
 * regardless of whether a live frame (paused_L) is available.  The native
 * transport gates its reader thread on this so exception-stop inspection requests
 * are queued to the blocked exec thread rather than dispatched inline. */
int dap_lua_is_paused(void);

#ifdef __cplusplus
}
#endif
