/* runtime/host/src/dap/master_hook.c
 *
 * Master hook dispatcher — compiled for WASM host and RV32 guest.
 *
 * For the WASM path: fc_dap_should_break checks the breakpoint table directly
 * (same address space); fc_dap_pause_loop suspends via emscripten_sleep.
 *
 * For the emulated RV32 path: master_hook_ecall.c overrides fc_dap_should_break
 * and fc_dap_pause_loop with ECALL stubs; the host handles all step logic.
 */

#include <stddef.h>
#include <stdint.h>

#include "lua.h"

#include "master_hook.h"

hook_config_t fc_master_hook_cfg = {0};

static int dap_call_depth(lua_State *L) {
    int depth = 0;
    lua_Debug ar;
    while (lua_getstack(L, depth, &ar))
        depth++;
    return depth;
}

static void dap_dispatch(lua_State *L, lua_Debug *ar) {
    if (!fc_master_hook_cfg.dap_enabled)
        return;
    /* Skip all breakpoint/step logic while handle_evaluate is running a pcall.
     * If we let the hook fire and yield, luaG_traceexec would set L->status =
     * LUA_YIELD inside the pcall, which propagates as rc=1 back to the caller.
     * We do NOT call lua_sethook to disable the hook (that clears ci->u.l.trap
     * and breaks subsequent hook delivery after resume). */
    if (fc_master_hook_cfg.dap_evaluating)
        return;

    if (fc_master_hook_cfg.dap_pending_pause) {
        fc_master_hook_cfg.dap_pending_pause = 0;
        if (fc_dap_pause_loop)
            fc_dap_pause_loop(L, ar);
        return;
    }

    if (ar->event == LUA_HOOKLINE && fc_master_hook_cfg.dap_step_mode != DAP_STEP_NONE) {
        int depth = dap_call_depth(L);
        bool should_pause = false;
        switch (fc_master_hook_cfg.dap_step_mode) {
        case DAP_STEP_IN:
            should_pause = true;
            break;
        case DAP_STEP_OVER:
            should_pause = depth <= fc_master_hook_cfg.dap_step_base_depth;
            break;
        case DAP_STEP_OUT:
            should_pause = depth < fc_master_hook_cfg.dap_step_base_depth;
            break;
        default:
            break;
        }
        if (should_pause) {
            fc_master_hook_cfg.dap_step_mode = DAP_STEP_NONE;
            if (fc_dap_pause_loop)
                fc_dap_pause_loop(L, ar);
            return;
        }
    }

    if (ar->event == LUA_HOOKLINE) {
        if (fc_dap_should_break && fc_dap_should_break(L, ar)) {
            if (fc_dap_pause_loop)
                fc_dap_pause_loop(L, ar);
        }
    }
}

void fc_consolelua_master_hook(lua_State *L, lua_Debug *ar) {
    if (fc_master_hook_cfg.dap_enabled)
        dap_dispatch(L, ar);
}

void fc_consolelua_master_hook_install(lua_State *L) {
    int mask = 0;
    if (fc_master_hook_cfg.dap_enabled)
        mask |= LUA_MASKLINE | LUA_MASKCALL | LUA_MASKRET;

    if (mask == 0)
        lua_sethook(L, NULL, 0, 0);
    else
        lua_sethook(L, fc_consolelua_master_hook, mask, 0);
}
