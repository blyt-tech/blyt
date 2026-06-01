/* runtime/guest/src/libblyt32lua/master_hook_ecall.c
 *
 * Strong definitions of fc_dap_should_break / fc_dap_pause_loop for the
 * emulated RV32 path (rv32emu + SDL2/libretro).
 *
 * The host (dap_server.c) handles session setup (initialize, setBreakpoints,
 * configurationDone) and step/continue commands.  When a breakpoint or step
 * condition fires, BLYT_ECALL_DAP_HOOK returns 1 (host has sent the "stopped"
 * event) and fc_dap_pause_loop runs the inspection loop guest-side:
 *
 *   fc_dap_pause_loop:
 *     loop:
 *       len = ECALL_DAP_RECV(buf) -- blocks until host delivers a command
 *       if len == 0: break        -- host received continue/step/disconnect
 *       dispatch(buf, L)          -- stackTrace/variables/evaluate via Lua API
 *       ECALL_DAP_SEND(response)  -- forward JSON response to VS Code
 *
 * Using direct lua_State* access gives the same inspection quality as the
 * WASM path.  No Lua symbols are needed on the host side.
 */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lauxlib.h"
#include "lua.h"

#include "master_hook.h"

#define BLYT_ECALL_DAP_HOOK 3
#define BLYT_ECALL_DAP_SEND 4
#define BLYT_ECALL_DAP_RECV 5
#define BLYT_ECALL_DAP_EXCEPTION 6
#define BLYT_ECALL_DAP_GET_CONDITION 7
#define BLYT_ECALL_DAP_CONDITION_RESULT 8

#define MAX_FRAMES 32
#define MAX_VARS 64
#define MAX_SOURCE 512
#define MSG_MAX (32 * 1024)

/* ── ECALL helpers ─────────────────────────────────────────────────────────── */

static int ecall_dap_hook(const char *src, int src_len, int line, int depth) {
    register long a0 __asm__("a0") = (long)src;
    register long a1 __asm__("a1") = (long)src_len;
    register long a2 __asm__("a2") = (long)line;
    register long a3 __asm__("a3") = (long)depth;
    register long a7 __asm__("a7") = BLYT_ECALL_DAP_HOOK;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a3), "r"(a7) : "memory");
    return (int)a0;
}

static void ecall_dap_send(const char *json) {
    size_t len = strlen(json);
    register long a0 __asm__("a0") = (long)json;
    register long a1 __asm__("a1") = (long)len;
    register long a7 __asm__("a7") = BLYT_ECALL_DAP_SEND;
    __asm__ volatile("ecall" : : "r"(a0), "r"(a1), "r"(a7) : "memory");
}

static int ecall_dap_recv(char *buf, int max_len) {
    register long a0 __asm__("a0") = (long)buf;
    register long a1 __asm__("a1") = (long)(unsigned)max_len;
    register long a7 __asm__("a7") = BLYT_ECALL_DAP_RECV;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a7) : "memory");
    return (int)a0;
}

static int ecall_dap_exception(const char *msg, int msg_len, int is_uncaught) {
    register long a0 __asm__("a0") = (long)msg;
    register long a1 __asm__("a1") = (long)msg_len;
    register long a2 __asm__("a2") = (long)is_uncaught;
    register long a7 __asm__("a7") = BLYT_ECALL_DAP_EXCEPTION;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return (int)a0;
}

static int ecall_dap_get_condition(char *buf, int max_len) {
    register long a0 __asm__("a0") = (long)buf;
    register long a1 __asm__("a1") = (long)(unsigned)max_len;
    register long a7 __asm__("a7") = BLYT_ECALL_DAP_GET_CONDITION;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a7) : "memory");
    return (int)a0;
}

static int ecall_dap_condition_result(int result) {
    register long a0 __asm__("a0") = (long)result;
    register long a7 __asm__("a7") = BLYT_ECALL_DAP_CONDITION_RESULT;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a7) : "memory");
    return (int)a0;
}

/* ── Simple JSON helpers (no dynamic allocation) ───────────────────────────── */

static int jget_int(const char *buf, const char *key, int def) {
    char k[64];
    snprintf(k, sizeof k, "\"%s\"", key);
    const char *p = strstr(buf, k);
    if (!p)
        return def;
    p = strchr(p, ':');
    if (!p)
        return def;
    p++;
    while (*p == ' ' || *p == '\t')
        p++;
    return (*p == '"') ? def : (int)strtol(p, NULL, 10);
}

static int jget_str(const char *buf, const char *key, char *out, size_t n) {
    char k[64];
    snprintf(k, sizeof k, "\"%s\"", key);
    const char *p = strstr(buf, k);
    if (!p)
        return 0;
    p = strchr(p, ':');
    if (!p)
        return 0;
    p++;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p != '"')
        return 0;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < n) {
        if (*p == '\\' && p[1]) {
            out[i++] = p[1];
            p += 2;
        } else
            out[i++] = *p++;
    }
    out[i] = 0;
    return 1;
}

/* ── Response helpers ──────────────────────────────────────────────────────── */

static int g_seq = 1000;
static char g_out[MSG_MAX];

static void send_resp(int req_seq, const char *cmd, int ok, const char *body) {
    if (ok)
        snprintf(g_out, sizeof g_out,
                 "{\"seq\":%d,\"type\":\"response\",\"request_seq\":%d,"
                 "\"command\":\"%s\",\"success\":true,\"body\":%s}",
                 g_seq++, req_seq, cmd, body ? body : "{}");
    else
        snprintf(g_out, sizeof g_out,
                 "{\"seq\":%d,\"type\":\"response\",\"request_seq\":%d,"
                 "\"command\":\"%s\",\"success\":false,\"message\":\"%s\"}",
                 g_seq++, req_seq, cmd, body ? body : "");
    ecall_dap_send(g_out);
}

/* ── Variable appender ─────────────────────────────────────────────────────── */

static int append_var(char *buf, size_t rem, const char *vn, lua_State *L, int first) {
    const char *vt = luaL_typename(L, -1);
    char val[256] = {0};
    if (lua_isstring(L, -1) || lua_isnumber(L, -1)) {
        const char *s = lua_tostring(L, -1);
        snprintf(val, sizeof val, "%s", s ? s : "?");
    } else {
        snprintf(val, sizeof val, "%s", vt);
    }
    for (char *q = val; *q; q++)
        if (*q == '"' || *q == '\\')
            *q = '_';
    return snprintf(buf, rem,
                    "%s{\"name\":\"%s\",\"value\":\"%s\","
                    "\"type\":\"%s\",\"variablesReference\":0}",
                    first ? "" : ",", vn, val, vt);
}

/* ── Request handlers ──────────────────────────────────────────────────────── */

static void on_stack_trace(int seq, lua_State *L) {
    static char body[MSG_MAX];
    int off = snprintf(body, sizeof body, "{\"stackFrames\":[");
    int frame = 0;
    lua_Debug ar2;
    while (frame < MAX_FRAMES && lua_getstack(L, frame, &ar2)) {
        lua_getinfo(L, "Snl", &ar2);
        const char *name = ar2.name ? ar2.name : (ar2.what ? ar2.what : "?");
        const char *s = ar2.source ? ar2.source : "?";
        if (*s == '@')
            s++;
        off += snprintf(body + off, sizeof(body) - (size_t)off,
                        "%s{\"id\":%d,\"name\":\"%s\","
                        "\"source\":{\"path\":\"%s\"},\"line\":%d,\"column\":1}",
                        frame ? "," : "", frame, name, s, ar2.currentline);
        frame++;
    }
    snprintf(body + off, sizeof(body) - (size_t)off, "],\"totalFrames\":%d}", frame);
    send_resp(seq, "stackTrace", 1, body);
}

static void on_scopes(int seq, const char *msg) {
    int frame_id = jget_int(msg, "frameId", 0);
    char body[256];
    snprintf(body, sizeof body,
             "{\"scopes\":[{\"name\":\"Locals\","
             "\"variablesReference\":%d,\"expensive\":false}]}",
             frame_id + 1);
    send_resp(seq, "scopes", 1, body);
}

static void on_variables(int seq, const char *msg, lua_State *L) {
    int vref = jget_int(msg, "variablesReference", 0);
    int frame_id = vref - 1;
    static char body[MSG_MAX];
    int off = snprintf(body, sizeof body, "{\"variables\":[");
    lua_Debug ar2;
    if (frame_id >= 0 && lua_getstack(L, frame_id, &ar2)) {
        int first = 1;
        const char *vn;
        int idx = 1;
        while ((vn = lua_getlocal(L, &ar2, idx)) != NULL && idx <= MAX_VARS) {
            if (vn[0] != '(') {
                off += append_var(body + off, sizeof(body) - (size_t)off, vn, L, first);
                first = 0;
            }
            lua_pop(L, 1);
            idx++;
        }
        lua_getinfo(L, "f", &ar2);
        int fn = lua_gettop(L), uvi = 1;
        while (uvi <= MAX_VARS) {
            vn = lua_getupvalue(L, fn, uvi++);
            if (!vn)
                break;
            if (strcmp(vn, "_ENV") != 0) {
                off += append_var(body + off, sizeof(body) - (size_t)off, vn, L, first);
                first = 0;
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1); /* pop function */
    }
    snprintf(body + off, sizeof(body) - (size_t)off, "]}");
    send_resp(seq, "variables", 1, body);
}

static void on_evaluate(int seq, const char *msg, lua_State *L) {
    char expr[256] = {0};
    jget_str(msg, "expression", expr, sizeof expr);
    int frame_id = jget_int(msg, "frameId", 0);

    lua_Debug ar2;
    if (expr[0] == '\0' || !lua_getstack(L, frame_id, &ar2)) {
        send_resp(seq, "evaluate", 1, "{\"result\":\"?\",\"variablesReference\":0}");
        return;
    }

    static char chunk[320];
    snprintf(chunk, sizeof chunk, "return (%s)", expr);

    char val[256] = {0};
    if (luaL_loadstring(L, chunk) == LUA_OK) {
        /* Inject frame locals into _ENV so expressions like "x + 1" work. */
        lua_newtable(L);
        lua_newtable(L);
        lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
        lua_setfield(L, -2, "__index");
        lua_setmetatable(L, -2);
        int li = 1;
        const char *vn;
        while ((vn = lua_getlocal(L, &ar2, li++)) != NULL)
            lua_setfield(L, -2, vn);
        lua_setupvalue(L, -2, 1);
        if (lua_pcall(L, 0, 1, 0) == LUA_OK) {
            int t = lua_type(L, -1);
            if (t == LUA_TNIL)
                snprintf(val, sizeof val, "nil");
            else if (t == LUA_TBOOLEAN)
                snprintf(val, sizeof val, "%s", lua_toboolean(L, -1) ? "true" : "false");
            else if (t == LUA_TNUMBER || t == LUA_TSTRING)
                snprintf(val, sizeof val, "%s", lua_tostring(L, -1));
            else
                snprintf(val, sizeof val, "%s", luaL_typename(L, -1));
            lua_pop(L, 1);
        } else {
            snprintf(val, sizeof val, "?");
            lua_pop(L, 1);
        }
    } else {
        snprintf(val, sizeof val, "?");
        lua_pop(L, 1);
    }

    for (char *q = val; *q; q++)
        if (*q == '"' || *q == '\\')
            *q = '_';
    char body[320];
    snprintf(body, sizeof body, "{\"result\":\"%s\",\"variablesReference\":0}", val);
    send_resp(seq, "evaluate", 1, body);
}

static void on_threads(int seq) {
    send_resp(seq, "threads", 1, "{\"threads\":[{\"id\":1,\"name\":\"cart\"}]}");
}

/* ── Call depth for step checking ──────────────────────────────────────────── */

static int hook_call_depth(lua_State *L) {
    int depth = 0;
    lua_Debug ar;
    while (lua_getstack(L, depth, &ar))
        depth++;
    return depth;
}

/* ── Public callbacks (strong definitions override master_hook.h weak decls) ─ */

int blyt_dap_active(void) {
    return ecall_dap_hook(NULL, 0, -1, 0);
}

bool fc_dap_should_break(lua_State *L, lua_Debug *ar) {
    lua_getinfo(L, "Sl", ar);

    const char *src = ar->source ? ar->source : "?";
    if (*src == '@')
        src++;
    int src_len = (int)strlen(src);
    int line = ar->currentline;
    int depth = hook_call_depth(L);

    int result = ecall_dap_hook(src, src_len, line, depth);
    if (result == 2) {
        /* Conditional breakpoint: fetch condition, evaluate in Lua, report back. */
        static char cond_buf[256];
        int clen = ecall_dap_get_condition(cond_buf, (int)sizeof(cond_buf) - 1);
        if (clen <= 0) {
            ecall_dap_condition_result(0);
            return false;
        }
        cond_buf[clen] = '\0';

        static char chunk[320];
        snprintf(chunk, sizeof chunk, "return (%s)", cond_buf);

        int cond_val = 0;
        if (luaL_loadstring(L, chunk) == LUA_OK) {
            lua_newtable(L);
            lua_newtable(L);
            lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
            lua_setfield(L, -2, "__index");
            lua_setmetatable(L, -2);
            /* Inject upvalues (skip _ENV), then locals (override upvalues). */
            lua_getinfo(L, "f", ar);
            {
                int ui = 1;
                const char *uname;
                while ((uname = lua_getupvalue(L, -1, ui++)) != NULL) {
                    if (strcmp(uname, "_ENV") != 0)
                        lua_setfield(L, -3, uname);
                    else
                        lua_pop(L, 1);
                }
            }
            lua_pop(L, 1);
            int li = 1;
            const char *vn;
            while ((vn = lua_getlocal(L, ar, li++)) != NULL)
                lua_setfield(L, -2, vn);
            lua_setupvalue(L, -2, 1); /* chunk._ENV = env */
            if (lua_pcall(L, 0, 1, 0) == LUA_OK) {
                cond_val = lua_toboolean(L, -1);
                lua_pop(L, 1);
            } else {
                lua_pop(L, 1);
            }
        } else {
            lua_pop(L, 1);
        }

        int should_pause = ecall_dap_condition_result(cond_val);
        return should_pause != 0;
    }
    return result != 0;
}

/* Call this from an xpcall error handler to notify the DAP server of a Lua
 * exception.  Returns non-zero if the debugger paused. */
int blyt_dap_report_exception(lua_State *L, int is_uncaught) {
    const char *msg = lua_tostring(L, -1);
    if (!msg)
        msg = "(error)";
    int msg_len = (int)strlen(msg);
    return ecall_dap_exception(msg, msg_len, is_uncaught);
}

/* Inspect loop: called when fc_dap_should_break returned true.
 * The host has already emitted the "stopped" event.  We receive inspection
 * commands via BLYT_ECALL_DAP_RECV and respond via BLYT_ECALL_DAP_SEND until
 * the host signals continue/step (RECV returns 0). */
void fc_dap_pause_loop(lua_State *L, lua_Debug *ar) {
    (void)ar;
    static char recv_buf[MSG_MAX];

    for (;;) {
        int len = ecall_dap_recv(recv_buf, (int)sizeof(recv_buf) - 1);
        if (len <= 0)
            break; /* host sent continue/step/disconnect */

        recv_buf[len] = '\0';
        int seq = jget_int(recv_buf, "seq", 0);
        char cmd[64] = {0};
        jget_str(recv_buf, "command", cmd, sizeof cmd);

        if (strcmp(cmd, "stackTrace") == 0)
            on_stack_trace(seq, L);
        else if (strcmp(cmd, "scopes") == 0)
            on_scopes(seq, recv_buf);
        else if (strcmp(cmd, "variables") == 0)
            on_variables(seq, recv_buf, L);
        else if (strcmp(cmd, "evaluate") == 0)
            on_evaluate(seq, recv_buf, L);
        else if (strcmp(cmd, "threads") == 0)
            on_threads(seq);
        else
            send_resp(seq, cmd, 0, "handled by host");
    }
}
