/* runtime/host/src/dap/master_hook.h
 *
 * Single lua_sethook slot dispatcher for the blyt DAP debugger.
 * Compiled for both the WASM host and the RV32 guest (libblyt32lua.so).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lua.h"

typedef enum {
    DAP_STEP_NONE = 0,
    DAP_STEP_OVER = 1, /* next:   pause on next line at <= current depth */
    DAP_STEP_IN = 2, /* stepIn: pause on next line at any depth */
    DAP_STEP_OUT = 3, /* stepOut: pause on next line at < current depth */
    DAP_STEP_PAUSE = 4, /* pause:  stop on next line event */
} dap_step_mode_t;

typedef struct {
    bool dap_enabled;
    dap_step_mode_t dap_step_mode;
    int dap_step_base_depth;
    int dap_pending_pause;
    int dap_evaluating; /* set while evaluate pcall runs; hook skips to preserve trap state */
    void *dap_state; /* opaque pointer owned by the transport */
} hook_config_t;

extern hook_config_t fc_master_hook_cfg;

/* Recompute the dispatch mask and (re)install the master hook. */
void fc_consolelua_master_hook_install(lua_State *L);

/* The dispatcher itself. */
void fc_consolelua_master_hook(lua_State *L, lua_Debug *ar);

/* Transport-supplied callbacks (weak so the dispatcher compiles without them). */
__attribute__((weak)) bool fc_dap_should_break(lua_State *L, lua_Debug *ar);
__attribute__((weak)) void fc_dap_pause_loop(lua_State *L, lua_Debug *ar);
