/* runtime/host/src/dap/dap_transport_tcp_lua.h
 *
 * Native (SDL2 / libretro) TCP transport for the host-side Lua DAP debugger —
 * the block-a-thread half of issue #234, serving the native host-Lua runner
 * (cart_run_hostlua.c).  Distinct `fc_hostlua_dap_*` namespace so it links
 * cleanly alongside dap_server.c (the emulated RV32 ECALL path, which owns the
 * fc_dap_* / fc_consolelua_dap_* symbols) in the same blytdebug binary.
 *
 * The master-hook callbacks fc_dap_should_break / fc_dap_pause_loop (declared in
 * master_hook.h) are also defined by this TU in the native host-Lua build.
 */

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bind 127.0.0.1:port (0 = OS-assigned), spawn the reader pthread, and reset the
 * shared inspection core.  Returns the actual bound port (>0), or -1 on failure. */
int fc_hostlua_dap_listen(int port);

/* Block until the client sends configurationDone (or the server shuts down /
 * never connects).  Returns non-zero once configuration is done, 0 otherwise. */
int fc_hostlua_dap_wait_ready(void);

/* Stop the reader thread, drop the client, and release resources.  Idempotent. */
void fc_hostlua_dap_shutdown(void);

/* Test-and-clear the pending-restart flag (issue #257): non-zero once the client
 * has sent a `restart` request.  The host-Lua run loop polls this at frame entry
 * to trigger a VM rebuild (BLYT_RUN_RESTART). */
int fc_hostlua_dap_restart_pending(void);

/* Report a Lua error and block the exec thread on the exception stop if an
 * exception breakpoint filter matches (issue #257).  Returns 1 if it paused
 * (client resumed/disconnected), 0 if no filter matched. */
int fc_hostlua_dap_report_exception(const char *msg, int is_uncaught);

#ifdef __cplusplus
}
#endif
