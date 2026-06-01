/* runtime/host/src/dap/dap_server.h
 *
 * DAP server interface — implemented by dap_server.c (TCP+pthread, SDL2/libretro)
 * and dap_transport_wasm.c (Emscripten WebSocket, WASM).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Start the DAP server.
 *
 * TCP version  (dap_server.c):       binds 127.0.0.1:port (0 = OS-assigned);
 *                                    spawns a pthread; returns actual port.
 * WASM version (dap_transport_wasm.c): connects outbound WebSocket to the blyt-run
 *                                    relay at the given port; returns that port.
 *
 * Returns the port in use (>0) on success, -1 on failure. */
int fc_consolelua_dap_listen(int port);

/* Stop the server and free all resources. Idempotent. */
void fc_consolelua_dap_shutdown(void);

/* Returns non-zero once the DAP client has sent configurationDone. */
int fc_dap_configuration_done(void);

/* Process any pending incoming DAP messages.
 * WASM: drains the WebSocket message queue (call once per frame, outside Lua hooks).
 * TCP (SDL2/libretro): no-op — the dedicated thread handles polling. */
void fc_dap_poll_messages(void);

/* Consulted by master_hook on every LUA_HOOKLINE event (WASM path).
 * Returns true if execution should stop here (breakpoint or step condition). */
struct lua_State;
struct lua_Debug;
bool fc_dap_should_break(struct lua_State *L, struct lua_Debug *ar);

/* Called by master_hook when a pause condition fires (WASM path).
 * Sets paused state, emits "stopped" event, then yields the Lua coroutine
 * via lua_yield().  After this returns, luaG_traceexec throws LUA_YIELD;
 * lua_resume() in wasm_lua_loop catches it.  Call fc_dap_do_resume() before
 * the next lua_resume() to apply the step mode and send "continued". */
void fc_dap_pause_loop(struct lua_State *L, struct lua_Debug *ar);

/* Returns non-zero (and clears the flag) if the last LUA_YIELD from
 * lua_resume() was initiated by fc_dap_pause_loop (a DAP pause), not by
 * the normal coroutine.yield() at end of each game frame. */
int fc_dap_hook_yielded(void);

/* Returns non-zero if the DAP client has sent continue/next/stepIn/stepOut. */
int fc_dap_continue_pending(void);

/* Apply pending step mode and send the "continued" event.  Call this from
 * wasm_lua_loop just before resuming the coroutine after a DAP pause. */
void fc_dap_do_resume(void);

/* Emitted when a new source is (re)loaded. */
void fc_dap_emit_loaded_source(const char *source_path);

/* Emit a DAP 'output' event — routes to VS Code's Debug Console. */
void fc_dap_output(const char *msg);

/* Called from the BLYT_ECALL_DAP_HOOK handler for the emulated path.
 *
 * Checks breakpoints + pending step mode against source/line/depth.
 * Non-blocking: emits "stopped" and returns 1 if the guest should pause,
 * 0 if execution should continue. */
int fc_dap_check_hook_line(const char *source, int line, int depth);

/* BLYT_ECALL_DAP_SEND: forward guest-built JSON to the connected DAP client. */
void fc_dap_host_send(const char *json, size_t len);

/* BLYT_ECALL_DAP_RECV: block until VS Code sends an inspection command.
 * Writes the command JSON to buf (max_len bytes incl. NUL).
 * Returns message length, or 0 when the guest should resume (continue/disconnect). */
int fc_dap_host_recv(char *buf, size_t max_len);

/* Block until the client sends configurationDone, or the server shuts down.
 * Returns non-zero if configurationDone was received; 0 if shutting down. */
int fc_dap_wait_configuration_done(void);

/* Returns non-zero (and clears the flag) if the client sent a restart request.
 * Checked at the start of blyt_session_run_frame() to trigger BLYT_RUN_RESTART. */
int fc_dap_is_restart_pending(void);

/* Copy the pending conditional breakpoint expression into buf (max n bytes).
 * Called from BLYT_ECALL_DAP_GET_CONDITION (ECALL 7) after ECALL 3 returns 2.
 * Returns the expression length. */
int fc_dap_get_condition(char *buf, size_t n);

/* Report the Lua condition evaluation result back to the host.
 * Called from BLYT_ECALL_DAP_CONDITION_RESULT (ECALL 8).
 * Returns 1 and emits "stopped" if result is true; 0 otherwise. */
int fc_dap_on_condition_result(int result);

/* Returns current exception filter: 0=none, 1=uncaught, 2=all. */
int fc_dap_exception_filter(void);

/* Call when the guest catches a Lua exception.  is_uncaught=1 when the error
 * would propagate to the top level.  Emits "stopped" if the filter matches.
 * Returns 1 if the debugger paused; 0 if execution should continue normally. */
int fc_dap_on_exception(const char *msg, int is_uncaught);

#ifdef __cplusplus
}
#endif
