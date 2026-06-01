/* runtime/host/src/dap/dap_transport_wasm.c
 *
 * DAP transport for the WASM frontend.
 *
 * The WASM binary connects outbound WebSocket to the blyt-run relay server at
 * ws://127.0.0.1:<relay_port>/dap.  The relay bridges to the external DAP
 * client (VS Code extension or dap_test.mjs test driver).
 *
 * Blocking in fc_dap_pause_loop relies on Emscripten ASYNCIFY: emscripten_sleep
 * suspends the WASM coroutine and returns control to the browser/Node.js event
 * loop, which delivers incoming WebSocket messages.  Each 1 ms tick checks
 * g_dap.continue_pending; when set, execution resumes.
 *
 * The lua_State* passed to fc_dap_pause_loop is a real host pointer (Lua runs
 * natively in WASM), enabling full stack-trace and variable inspection.
 *
 * Protocol: raw JSON strings over WebSocket (no Content-Length framing;
 * WebSocket frames are already self-delimiting).
 *
 * Compiled only for Emscripten builds (BLYT_DAP + __EMSCRIPTEN__).
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <emscripten.h>

#include "lauxlib.h"
#include "lstate.h" /* TStatus, L->status — needed to temp-clear yield flag during eval pcall */
#include "lua.h"

#include "dap_server.h"
#include "master_hook.h"

#define MAX_BREAKPOINTS 256
#define MAX_CONDITION 256
#define MAX_FRAMES 64
#define MAX_VARS 64
#define MAX_SOURCE_PATH 1024
#define MAX_SOURCES 64
#define MAX_MSG (64 * 1024)

/* ── State ─────────────────────────────────────────────────────────────────── */

typedef struct {
    char source[MAX_SOURCE_PATH];
    int line;
    int id;
    char condition[MAX_CONDITION];
} wdap_bp_t;

static struct {
    int connected;
    int configuration_done;
    int seq;

    wdap_bp_t bps[MAX_BREAKPOINTS];
    int n_bps;
    int next_bp_id;

    lua_State *paused_L;
    int paused;
    int continue_pending;

    int pending_step_mode;
    int pending_step_base_depth;
    int pending_pause;
    int hook_yielded; /* set by fc_dap_pause_loop; cleared by fc_dap_hook_yielded */

    int restart_pending;
    char loaded_sources[MAX_SOURCES][MAX_SOURCE_PATH];
    int n_sources;
    int exception_filter;
    int hit_bp_id; /* id of the breakpoint that caused the current pause, or 0 */
} g_wdap;

/* ── JSON helpers ──────────────────────────────────────────────────────────── */

static int json_get_int(const char *buf, const char *key, int def) {
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
    return atoi(p);
}

static int json_get_string(const char *buf, const char *key, char *out, size_t n) {
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
        } else {
            out[i++] = *p++;
        }
    }
    out[i] = 0;
    return 1;
}

static int json_get_int_array(const char *buf, const char *key, int *out, int max) {
    char k[64];
    snprintf(k, sizeof k, "\"%s\"", key);
    const char *p = strstr(buf, k);
    if (!p)
        return 0;
    p = strchr(p, '[');
    if (!p)
        return 0;
    p++;
    int n = 0;
    while (*p && *p != ']' && n < max) {
        while (*p == ' ' || *p == ',' || *p == '\t' || *p == '\n')
            p++;
        if (*p == ']')
            break;
        out[n++] = atoi(p);
        while (*p && *p != ',' && *p != ']')
            p++;
    }
    return n;
}

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

/* clang-format on */

static void send_json(const char *json) {
    wdap_ws_send_js(json);
}

static void send_response(int request_seq, const char *command, int success,
                          const char *body_or_msg) {
    static char buf[MAX_MSG];
    int seq = ++g_wdap.seq;
    if (success) {
        snprintf(buf, sizeof buf,
                 "{\"seq\":%d,\"type\":\"response\",\"request_seq\":%d,"
                 "\"command\":\"%s\",\"success\":true,\"body\":%s}",
                 seq, request_seq, command, body_or_msg ? body_or_msg : "{}");
    } else {
        snprintf(buf, sizeof buf,
                 "{\"seq\":%d,\"type\":\"response\",\"request_seq\":%d,"
                 "\"command\":\"%s\",\"success\":false,\"message\":\"%s\"}",
                 seq, request_seq, command, body_or_msg ? body_or_msg : "");
    }
    send_json(buf);
}

static void send_event(const char *event, const char *body) {
    static char buf[MAX_MSG];
    int seq = ++g_wdap.seq;
    snprintf(buf, sizeof buf, "{\"seq\":%d,\"type\":\"event\",\"event\":\"%s\",\"body\":%s}", seq,
             event, body ? body : "{}");
    send_json(buf);
}

/* ── Request handlers ──────────────────────────────────────────────────────── */

static void handle_initialize(int seq, const char *args) {
    (void)args;
    const char *body =
        "{\"supportsConfigurationDoneRequest\":true,"
        "\"supportsLoadedSourcesRequest\":true,"
        "\"supportsRestartRequest\":true,"
        "\"supportsStepBack\":false,"
        "\"supportsTerminateRequest\":true,"
        "\"supportsConditionalBreakpoints\":true,"
        "\"supportsExceptionBreakpoints\":true,"
        "\"exceptionBreakpointFilters\":["
        "{\"filter\":\"uncaught\",\"label\":\"Uncaught Exceptions\",\"default\":false},"
        "{\"filter\":\"all\",\"label\":\"All Exceptions\",\"default\":false}"
        "]}";
    send_response(seq, "initialize", 1, body);
    send_event("initialized", "{}");
}

static void handle_launch(int seq, const char *args) {
    (void)args;
    send_response(seq, "launch", 1, "{}");
}

static void handle_configuration_done(int seq, const char *args) {
    (void)args;
    g_wdap.configuration_done = 1;
    send_response(seq, "configurationDone", 1, "{}");
}

/* Parse "breakpoints":[{"line":N,"condition":"..."},...] — preferred VS Code format. */
static int json_get_bp_array(const char *buf, int *lines, char (*conds)[MAX_CONDITION], int max) {
    const char *p = strstr(buf, "\"breakpoints\"");
    if (!p)
        return 0;
    p = strchr(p, '[');
    if (!p)
        return 0;
    int n = 0;
    p++;
    while (*p && n < max) {
        while (*p && *p != '{' && *p != ']')
            p++;
        if (!*p || *p == ']')
            break;
        const char *obj = p;
        int depth = 1;
        p++;
        while (*p && depth > 0) {
            if (*p == '{')
                depth++;
            else if (*p == '}')
                depth--;
            if (depth > 0)
                p++;
        }
        const char *lp = strstr(obj, "\"line\"");
        if (!lp || lp > p) {
            if (*p)
                p++;
            continue;
        }
        lp = strchr(lp + 6, ':');
        if (!lp || lp > p) {
            if (*p)
                p++;
            continue;
        }
        lp++;
        while (*lp == ' ' || *lp == '\t')
            lp++;
        lines[n] = atoi(lp);
        if (conds) {
            conds[n][0] = '\0';
            const char *cp = strstr(obj, "\"condition\"");
            if (cp && cp < p) {
                cp = strchr(cp + 11, ':');
                if (cp && cp < p) {
                    cp++;
                    while (*cp == ' ' || *cp == '\t')
                        cp++;
                    if (*cp == '"') {
                        cp++;
                        size_t i = 0;
                        while (*cp && *cp != '"' && i + 1 < MAX_CONDITION) {
                            if (*cp == '\\' && cp[1]) {
                                conds[n][i++] = cp[1];
                                cp += 2;
                            } else {
                                conds[n][i++] = *cp++;
                            }
                        }
                        conds[n][i] = '\0';
                    }
                }
            }
        }
        n++;
        if (*p)
            p++;
    }
    return n;
}

static void handle_set_breakpoints(int seq, const char *args) {
    static char source[MAX_SOURCE_PATH];
    source[0] = '\0';
    json_get_string(args, "path", source, sizeof source);

    int lines[MAX_BREAKPOINTS];
    char conds[MAX_BREAKPOINTS][MAX_CONDITION];
    int n = json_get_bp_array(args, lines, conds, MAX_BREAKPOINTS);
    if (n == 0) {
        n = json_get_int_array(args, "lines", lines, MAX_BREAKPOINTS);
        for (int i = 0; i < n; i++)
            conds[i][0] = '\0';
    }

    /* Replace all breakpoints for this source. */
    int kept = 0;
    for (int i = 0; i < g_wdap.n_bps; i++) {
        if (strcmp(g_wdap.bps[i].source, source) != 0) {
            if (kept != i)
                g_wdap.bps[kept] = g_wdap.bps[i];
            kept++;
        }
    }
    g_wdap.n_bps = kept;
    for (int i = 0; i < n && g_wdap.n_bps < MAX_BREAKPOINTS; i++) {
        wdap_bp_t *bp = &g_wdap.bps[g_wdap.n_bps++];
        snprintf(bp->source, sizeof bp->source, "%s", source);
        bp->line = lines[i];
        bp->id = ++g_wdap.next_bp_id;
        snprintf(bp->condition, sizeof bp->condition, "%s", conds[i]);
    }

    static char body[MAX_MSG];
    int off = snprintf(body, sizeof body, "{\"breakpoints\":[");
    for (int i = 0; i < n; i++) {
        int fid = 0;
        for (int j = 0; j < g_wdap.n_bps; j++) {
            if (strcmp(g_wdap.bps[j].source, source) == 0 && g_wdap.bps[j].line == lines[i]) {
                fid = g_wdap.bps[j].id;
                break;
            }
        }
        off += snprintf(body + off, sizeof(body) - (size_t)off,
                        "%s{\"id\":%d,\"verified\":true,\"line\":%d}", i ? "," : "", fid, lines[i]);
    }
    snprintf(body + off, sizeof(body) - (size_t)off, "]}");
    send_response(seq, "setBreakpoints", 1, body);
}

static void handle_threads(int seq, const char *args) {
    (void)args;
    send_response(seq, "threads", 1, "{\"threads\":[{\"id\":1,\"name\":\"cart\"}]}");
}

static void handle_stack_trace(int seq, const char *args) {
    (void)args;
    lua_State *L = g_wdap.paused_L;
    if (!L) {
        send_response(seq, "stackTrace", 0, "not paused");
        return;
    }
    static char body[MAX_MSG];
    int off = snprintf(body, sizeof body, "{\"stackFrames\":[");
    int display = 0;
    int lua_frame = 0;
    lua_Debug ar;
    while (lua_frame < MAX_FRAMES && lua_getstack(L, lua_frame, &ar)) {
        lua_getinfo(L, "Snl", &ar);
        lua_frame++;
        /* Skip synthetic frames (inline strings, C functions).
         * Only show @-prefixed file-backed sources so VS Code gets real paths. */
        const char *src = ar.source ? ar.source : "";
        if (*src != '@')
            continue;
        src++; /* strip leading @ */
        const char *name = ar.name ? ar.name : (ar.what ? ar.what : "?");
        off += snprintf(body + off, sizeof(body) - (size_t)off,
                        "%s{\"id\":%d,\"name\":\"%s\","
                        "\"source\":{\"path\":\"%s\"},\"line\":%d,\"column\":1}",
                        display ? "," : "", display, name, src, ar.currentline);
        display++;
    }
    snprintf(body + off, sizeof(body) - (size_t)off, "],\"totalFrames\":%d}", display);
    send_response(seq, "stackTrace", 1, body);
}

/* Map a DAP frame id (counting only @-source Lua frames) to the actual Lua
 * stack level so lua_getstack/lua_getlocal see the right activation record.
 * handle_stack_trace skips C frames when assigning DAP frame ids; without
 * this mapping, evaluate/variables land on the wrong (C) frame and miss locals. */
static int lua_level_for_dap_frame(lua_State *L, int dap_frame_id) {
    lua_Debug ar;
    int display = 0;
    for (int level = 0; lua_getstack(L, level, &ar); level++) {
        lua_getinfo(L, "S", &ar);
        const char *src = ar.source ? ar.source : "";
        if (*src != '@')
            continue;
        if (display == dap_frame_id)
            return level;
        display++;
    }
    return dap_frame_id; /* fallback: pass through */
}

static void handle_scopes(int seq, const char *args) {
    int frame_id = json_get_int(args, "frameId", 0);
    lua_State *L = g_wdap.paused_L;
    int lua_level = L ? lua_level_for_dap_frame(L, frame_id) : frame_id;
    char body[256];
    snprintf(body, sizeof body,
             "{\"scopes\":[{\"name\":\"Locals\",\"variablesReference\":%d,"
             "\"expensive\":false}]}",
             lua_level + 1);
    send_response(seq, "scopes", 1, body);
}

static int append_variable(char *buf, size_t remaining, const char *vn, lua_State *L, int first) {
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
    return snprintf(buf, remaining,
                    "%s{\"name\":\"%s\",\"value\":\"%s\","
                    "\"type\":\"%s\",\"variablesReference\":0}",
                    first ? "" : ",", vn, val, vt);
}

static void handle_variables(int seq, const char *args) {
    int vref = json_get_int(args, "variablesReference", 0);
    int frame_id = vref - 1;
    lua_State *L = g_wdap.paused_L;
    if (!L || frame_id < 0) {
        send_response(seq, "variables", 1, "{\"variables\":[]}");
        return;
    }
    static char body[MAX_MSG];
    int off = snprintf(body, sizeof body, "{\"variables\":[");
    lua_Debug ar;
    if (lua_getstack(L, frame_id, &ar)) {
        int first = 1;

        /* Local variables. */
        const char *vn;
        int idx = 1;
        while ((vn = lua_getlocal(L, &ar, idx)) != NULL && idx <= MAX_VARS) {
            if (vn[0] != '(') {
                off += append_variable(body + off, sizeof(body) - (size_t)off, vn, L, first);
                first = 0;
            }
            lua_pop(L, 1);
            idx++;
        }

        /* Upvalues of the current function (captures like outer locals). */
        lua_getinfo(L, "f", &ar); /* pushes function onto stack */
        int fn = lua_gettop(L);
        int uvi = 1;
        while (uvi <= MAX_VARS) {
            vn = lua_getupvalue(L, fn, uvi++);
            if (!vn)
                break;
            if (strcmp(vn, "_ENV") != 0) {
                off += append_variable(body + off, sizeof(body) - (size_t)off, vn, L, first);
                first = 0;
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1); /* pop function */
    }
    snprintf(body + off, sizeof(body) - (size_t)off, "]}");
    send_response(seq, "variables", 1, body);
}

/* Format the Lua value at stack top into val[0..n-1]. Does not pop. */
static void format_lua_val(lua_State *L, char *val, size_t n) {
    int t = lua_type(L, -1);
    if (t == LUA_TNIL)
        snprintf(val, n, "nil");
    else if (t == LUA_TBOOLEAN)
        snprintf(val, n, "%s", lua_toboolean(L, -1) ? "true" : "false");
    else if (t == LUA_TNUMBER || t == LUA_TSTRING)
        snprintf(val, n, "%s", lua_tostring(L, -1));
    else
        snprintf(val, n, "%s", luaL_typename(L, -1));
}

/* Direct local+upvalue scan for simple identifier expressions.
 * Used as a fallback when chunk _ENV injection is unavailable. */
static int lookup_var(lua_State *L, lua_Debug *ar, const char *name, char *val, size_t n) {
    /* Locals first. */
    for (int li = 1;; li++) {
        const char *vn = lua_getlocal(L, ar, li);
        if (!vn)
            break;
        if (strcmp(vn, name) == 0) {
            format_lua_val(L, val, n);
            lua_pop(L, 1);
            return 1;
        }
        lua_pop(L, 1);
    }
    /* Upvalues. */
    lua_getinfo(L, "f", ar);
    int fn = lua_gettop(L);
    int found = 0;
    for (int ui = 1;; ui++) {
        const char *uname = lua_getupvalue(L, fn, ui);
        if (!uname)
            break;
        if (strcmp(uname, name) == 0) {
            format_lua_val(L, val, n);
            found = 1;
        }
        lua_pop(L, 1);
        if (found)
            break;
    }
    lua_pop(L, 1); /* pop function */
    return found;
}

static void handle_evaluate(int seq, const char *args) {
    char expr[256] = {0};
    json_get_string(args, "expression", expr, sizeof expr);
    int frame_id = json_get_int(args, "frameId", 0);

    lua_State *L = g_wdap.paused_L;
    if (!L || expr[0] == '\0') {
        send_response(seq, "evaluate", 1, "{\"result\":\"(not paused)\",\"variablesReference\":0}");
        return;
    }

    lua_Debug ar;
    int lua_level = lua_level_for_dap_frame(L, frame_id);
    if (!lua_getstack(L, lua_level, &ar)) {
        send_response(seq, "evaluate", 1, "{\"result\":\"(no frame)\",\"variablesReference\":0}");
        return;
    }

    int saved_top = lua_gettop(L);
    char val[256] = {0};
    char chunk[320];
    snprintf(chunk, sizeof chunk, "return (%s)", expr);

    if (luaL_loadstring(L, chunk) == LUA_OK) {
        /* Build env = setmetatable({locals}, {__index=_G}) and set as _ENV. */
        lua_newtable(L);
        lua_newtable(L);
        lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
        lua_setfield(L, -2, "__index");
        lua_setmetatable(L, -2);
        {
            int li = 1;
            const char *vn;
            while ((vn = lua_getlocal(L, &ar, li++)) != NULL)
                lua_setfield(L, -2, vn);
        }
        lua_setupvalue(L, -2, 1); /* chunk._ENV = env */

        if (1) {
            /* Two issues to solve for pcall on a yielded coroutine:
             *   1. L->status == LUA_YIELD from the original pause.
             *      luaG_traceexec checks L->status after every hook call; if
             *      it sees YIELD it throws again — so pcall returns rc=1.
             *      Fix: temporarily clear status to LUA_OK.
             *   2. The line hook would call fc_dap_pause_loop → lua_yield which
             *      sets status back to YIELD and re-enters the throw path.
             *      Fix: dap_evaluating flag makes dap_dispatch return early.
             * After pcall we restore the status so lua_resume still works. */
            TStatus saved_status = L->status;
            L->status = LUA_OK;
            fc_master_hook_cfg.dap_evaluating = 1;
            int pcall_rc = lua_pcall(L, 0, 1, 0);
            fc_master_hook_cfg.dap_evaluating = 0;
            if (L->status == LUA_OK) /* pcall didn't kill the thread */
                L->status = saved_status;

            if (pcall_rc == LUA_OK) {
                format_lua_val(L, val, sizeof val);
            } else {
                const char *err = lua_tostring(L, -1);
                snprintf(val, sizeof val, "(error: %s)", err ? err : "?");
            }
        }
    } else {
        const char *err = lua_tostring(L, -1);
        snprintf(val, sizeof val, "(load: %s)", err ? err : "?");
    }

    lua_settop(L, saved_top); /* always restore stack */

    for (char *q = val; *q; q++)
        if (*q == '"' || *q == '\\')
            *q = '_';
    static char body[MAX_MSG];
    snprintf(body, sizeof body, "{\"result\":\"%s\",\"variablesReference\":0}", val);
    send_response(seq, "evaluate", 1, body);
}

static void handle_continue(int seq, const char *args) {
    (void)args;
    g_wdap.continue_pending = 1;
    g_wdap.pending_step_mode = 0;
    g_wdap.pending_pause = 0;
    send_response(seq, "continue", 1, "{\"allThreadsContinued\":true}");
}

static void handle_step(int seq, const char *args, int mode) {
    (void)args;
    /* Depth at pause time — used for step-over / step-out depth comparison. */
    int depth = 0;
    if (g_wdap.paused_L) {
        lua_Debug ar;
        while (lua_getstack(g_wdap.paused_L, depth, &ar))
            depth++;
    }
    g_wdap.pending_step_mode = mode;
    g_wdap.pending_step_base_depth = depth;
    g_wdap.pending_pause = 0;
    g_wdap.continue_pending = 1;
    const char *cmd = (mode == DAP_STEP_OVER) ? "next"
                      : (mode == DAP_STEP_IN) ? "stepIn"
                                              : "stepOut";
    send_response(seq, cmd, 1, "{\"allThreadsContinued\":true}");
}

static void handle_pause(int seq, const char *args) {
    (void)args;
    g_wdap.pending_pause = 1;
    send_response(seq, "pause", 1, "{}");
}

static void handle_disconnect(int seq, const char *args) {
    (void)args;
    send_response(seq, "disconnect", 1, "{}");
    g_wdap.continue_pending = 1;
    g_wdap.pending_step_mode = 0;
    g_wdap.pending_pause = 0;
}

static void handle_source(int seq, const char *args) {
    int ref = json_get_int(args, "sourceReference", -1);
    if (ref == 0) {
        /* sourceReference=0 means the file lives on disk at source.path.
         * Return success with no content so VS Code continues with navigation
         * but does NOT replace the editor buffer with an empty payload. */
        send_response(seq, "source", 1, "{}");
    } else {
        send_response(seq, "source", 0, "source not available");
    }
}

static void handle_restart(int seq, const char *args) {
    (void)args;
    g_wdap.n_bps = 0;
    g_wdap.n_sources = 0;
    g_wdap.configuration_done = 0;
    g_wdap.restart_pending = 1;
    g_wdap.continue_pending = 1;
    g_wdap.pending_step_mode = 0;
    g_wdap.pending_pause = 0;
    send_response(seq, "restart", 1, "{}");
    send_event("initialized", "{}");
}

static void handle_loaded_sources(int seq, const char *args) {
    (void)args;
    static char body[MAX_MSG];
    int off = snprintf(body, sizeof body, "{\"sources\":[");
    for (int i = 0; i < g_wdap.n_sources; i++) {
        off +=
            snprintf(body + off, sizeof(body) - (size_t)off, "%s{\"path\":\"%s\",\"name\":\"%s\"}",
                     i ? "," : "", g_wdap.loaded_sources[i], g_wdap.loaded_sources[i]);
    }
    snprintf(body + off, sizeof(body) - (size_t)off, "]}");
    send_response(seq, "loadedSources", 1, body);
}

static void handle_set_exception_breakpoints(int seq, const char *args) {
    int filter = 0;
    if (strstr(args, "\"all\""))
        filter = 2;
    else if (strstr(args, "\"uncaught\""))
        filter = 1;
    g_wdap.exception_filter = filter;
    send_response(seq, "setExceptionBreakpoints", 1, "{\"breakpoints\":[]}");
}

/* ── Dispatcher ────────────────────────────────────────────────────────────── */

static void dispatch(const char *msg) {
    int seq = json_get_int(msg, "seq", 0);
    char cmd[64];
    if (!json_get_string(msg, "command", cmd, sizeof cmd))
        return;

    if (strcmp(cmd, "initialize") == 0)
        handle_initialize(seq, msg);
    else if (strcmp(cmd, "launch") == 0)
        handle_launch(seq, msg);
    else if (strcmp(cmd, "configurationDone") == 0)
        handle_configuration_done(seq, msg);
    else if (strcmp(cmd, "setBreakpoints") == 0)
        handle_set_breakpoints(seq, msg);
    else if (strcmp(cmd, "setExceptionBreakpoints") == 0)
        handle_set_exception_breakpoints(seq, msg);
    else if (strcmp(cmd, "threads") == 0)
        handle_threads(seq, msg);
    else if (strcmp(cmd, "stackTrace") == 0)
        handle_stack_trace(seq, msg);
    else if (strcmp(cmd, "scopes") == 0)
        handle_scopes(seq, msg);
    else if (strcmp(cmd, "variables") == 0)
        handle_variables(seq, msg);
    else if (strcmp(cmd, "evaluate") == 0)
        handle_evaluate(seq, msg);
    else if (strcmp(cmd, "continue") == 0)
        handle_continue(seq, msg);
    else if (strcmp(cmd, "next") == 0)
        handle_step(seq, msg, DAP_STEP_OVER);
    else if (strcmp(cmd, "stepIn") == 0)
        handle_step(seq, msg, DAP_STEP_IN);
    else if (strcmp(cmd, "stepOut") == 0)
        handle_step(seq, msg, DAP_STEP_OUT);
    else if (strcmp(cmd, "pause") == 0)
        handle_pause(seq, msg);
    else if (strcmp(cmd, "restart") == 0)
        handle_restart(seq, msg);
    else if (strcmp(cmd, "disconnect") == 0)
        handle_disconnect(seq, msg);
    else if (strcmp(cmd, "terminate") == 0)
        handle_disconnect(seq, msg);
    else if (strcmp(cmd, "source") == 0)
        handle_source(seq, msg);
    else if (strcmp(cmd, "loadedSources") == 0)
        handle_loaded_sources(seq, msg);
    else
        send_response(seq, cmd, 0, "unknown command");
}

/* ── Message queue ─────────────────────────────────────────────────────────── */

/* clang-format off */
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

/* Drain the message queue and dispatch each JSON message. */
static void drain_queue(void) {
    while (wdap_queue_length() > 0) {
        char *json = wdap_dequeue_json();
        if (!json)
            break;
        dispatch(json);
        free(json);
    }
}

/* ── Public API ────────────────────────────────────────────────────────────── */

void fc_dap_poll_messages(void) {
    drain_queue();
}

int fc_consolelua_dap_listen(int relay_port) {
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
    return g_wdap.configuration_done;
}

/* ── Master hook callbacks (WASM direct-Lua path) ──────────────────────────── */

static const char *basename_of(const char *p) {
    const char *b = p;
    for (const char *q = p; *q; q++)
        if (*q == '/')
            b = q + 1;
    return b;
}

/* Evaluate a Lua condition expression; returns non-zero if truthy. */
/* Evaluate a Lua expression in the context of the paused frame (ar).
 * Locals from the frame are injected into the chunk's _ENV so that
 * expressions like "x > 10" work even when x is a local variable. */
static int eval_condition(lua_State *L, lua_Debug *ar, const char *cond) {
    char chunk[320];
    snprintf(chunk, sizeof chunk, "return (%s)", cond);
    if (luaL_loadstring(L, chunk) != LUA_OK) {
        lua_pop(L, 1);
        return 0;
    }
    /* Build env = setmetatable({}, {__index=_G}) so globals still work. */
    lua_newtable(L);
    lua_newtable(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
    /* Inject upvalues of the paused function (skip _ENV itself). */
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
    /* Inject locals (override upvalues if same name). */
    int idx = 1;
    const char *vn;
    while ((vn = lua_getlocal(L, ar, idx++)) != NULL)
        lua_setfield(L, -2, vn);
    lua_setupvalue(L, -2, 1); /* chunk._ENV = env */
    int result = 0;
    if (lua_pcall(L, 0, 1, 0) == LUA_OK) {
        result = lua_toboolean(L, -1);
        lua_pop(L, 1);
    } else {
        lua_pop(L, 1);
    }
    return result;
}

bool fc_dap_should_break(lua_State *L, lua_Debug *ar) {
    if (!wdap_is_connected_js())
        return false;
    if (g_wdap.pending_pause)
        return true;
    lua_getinfo(L, "Sl", ar);
    const char *src = ar->source ? ar->source : "";
    if (*src == '@')
        src++;
    fc_dap_emit_loaded_source(src);
    const char *src_base = basename_of(src);
    for (int i = 0; i < g_wdap.n_bps; i++) {
        if (g_wdap.bps[i].line != ar->currentline)
            continue;
        if (strcmp(basename_of(g_wdap.bps[i].source), src_base) != 0)
            continue;
        if (g_wdap.bps[i].condition[0]) {
            if (!eval_condition(L, ar, g_wdap.bps[i].condition))
                return false;
        }
        g_wdap.hit_bp_id = g_wdap.bps[i].id;
        return true;
    }
    return false;
}

void fc_dap_pause_loop(lua_State *L, lua_Debug *ar) {
    (void)ar;
    g_wdap.paused_L = L;
    g_wdap.paused = 1;
    g_wdap.continue_pending = 0;
    g_wdap.pending_pause = 0;
    g_wdap.hook_yielded = 1; /* signal to wasm_lua_loop */

    const char *reason = (fc_master_hook_cfg.dap_pending_pause)                ? "pause"
                         : (fc_master_hook_cfg.dap_step_mode != DAP_STEP_NONE) ? "step"
                                                                               : "breakpoint";
    char body[256];
    if (strcmp(reason, "breakpoint") == 0 && g_wdap.hit_bp_id > 0) {
        snprintf(body, sizeof body,
                 "{\"reason\":\"breakpoint\",\"threadId\":1,\"allThreadsStopped\":true,"
                 "\"hitBreakpointIds\":[%d]}",
                 g_wdap.hit_bp_id);
        g_wdap.hit_bp_id = 0;
    } else {
        snprintf(body, sizeof body, "{\"reason\":\"%s\",\"threadId\":1,\"allThreadsStopped\":true}",
                 reason);
    }
    send_event("stopped", body);

    /* Yield the Lua coroutine.  In the line-hook context lua_yield() returns 0
     * (does NOT throw): luaG_traceexec will set CIST_HOOKYIELD and throw
     * LUA_YIELD after this function returns, correctly preserving the VM PC so
     * execution resumes at the right instruction after fc_dap_do_resume(). */
    lua_yield(L, 0);
}

int fc_dap_hook_yielded(void) {
    if (g_wdap.hook_yielded) {
        g_wdap.hook_yielded = 0;
        return 1;
    }
    return 0;
}

int fc_dap_continue_pending(void) {
    return g_wdap.continue_pending;
}

void fc_dap_do_resume(void) {
    int step_mode = g_wdap.pending_step_mode;
    int step_base = g_wdap.pending_step_base_depth;
    g_wdap.paused_L = NULL;
    g_wdap.paused = 0;
    g_wdap.continue_pending = 0;

    fc_master_hook_cfg.dap_step_mode = (dap_step_mode_t)step_mode;
    fc_master_hook_cfg.dap_step_base_depth = step_base;
    fc_master_hook_cfg.dap_pending_pause = 0;

    send_event("continued", "{\"threadId\":1,\"allThreadsContinued\":true}");
}

void fc_dap_output(const char *msg) {
    if (!wdap_is_connected_js())
        return;
    static char body[MAX_MSG];
    char esc[MAX_MSG / 2];
    size_t j = 0;
    for (size_t i = 0; msg[i] && j + 4 < sizeof esc; i++) {
        if (msg[i] == '"') {
            esc[j++] = '\\';
            esc[j++] = '"';
        } else if (msg[i] == '\\') {
            esc[j++] = '\\';
            esc[j++] = '\\';
        } else if (msg[i] == '\n') {
            esc[j++] = '\\';
            esc[j++] = 'n';
        } else if (msg[i] == '\r') {
            esc[j++] = '\\';
            esc[j++] = 'r';
        } else {
            esc[j++] = msg[i];
        }
    }
    esc[j] = '\0';
    snprintf(body, sizeof body, "{\"category\":\"stdout\",\"output\":\"%s\\n\"}", esc);
    send_event("output", body);
}

void fc_dap_emit_loaded_source(const char *source_path) {
    int already = 0;
    for (int i = 0; i < g_wdap.n_sources; i++) {
        if (strcmp(g_wdap.loaded_sources[i], source_path) == 0) {
            already = 1;
            break;
        }
    }
    if (!already && g_wdap.n_sources < MAX_SOURCES)
        snprintf(g_wdap.loaded_sources[g_wdap.n_sources++], MAX_SOURCE_PATH, "%s", source_path);
    if (already)
        return;
    char body[MAX_SOURCE_PATH + 128];
    snprintf(body, sizeof body,
             "{\"reason\":\"changed\",\"source\":{\"path\":\"%s\",\"name\":\"%s\"}}", source_path,
             source_path);
    send_event("loadedSource", body);
}

int fc_dap_is_restart_pending(void) {
    /* WASM: no session restart; fc_dap_handle_restart() only resets DAP state. */
    return 0;
}

int fc_dap_get_condition(char *buf, size_t n) {
    /* WASM: conditions are evaluated directly in fc_dap_should_break; not used. */
    if (n > 0)
        buf[0] = '\0';
    return 0;
}

int fc_dap_on_condition_result(int result) {
    (void)result;
    return 0;
}

int fc_dap_exception_filter(void) {
    return g_wdap.exception_filter;
}

int fc_dap_on_exception(const char *msg, int is_uncaught) {
    if (!wdap_is_connected_js() || g_wdap.exception_filter == 0)
        return 0;
    if (g_wdap.exception_filter == 1 && !is_uncaught)
        return 0;
    g_wdap.paused_L = NULL; /* no Lua state at panic time */
    g_wdap.paused = 1;
    g_wdap.continue_pending = 0;
    g_wdap.hook_yielded = 1;
    static char body[256];
    char esc[200] = {0};
    if (msg) {
        size_t j = 0;
        for (size_t i = 0; msg[i] && j + 4 < sizeof esc; i++) {
            if (msg[i] == '"') {
                esc[j++] = '\\';
                esc[j++] = '"';
            } else if (msg[i] == '\\') {
                esc[j++] = '\\';
                esc[j++] = '\\';
            } else
                esc[j++] = msg[i];
        }
    }
    snprintf(body, sizeof body,
             "{\"reason\":\"exception\",\"description\":\"%s\","
             "\"threadId\":1,\"allThreadsStopped\":true}",
             esc);
    send_event("stopped", body);
    return 1;
}

int blyt_dap_report_exception(lua_State *L, int is_uncaught) {
    const char *msg = lua_tostring(L, -1);
    if (!msg)
        msg = "(error)";
    return fc_dap_on_exception(msg, is_uncaught);
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
