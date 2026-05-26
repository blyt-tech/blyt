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
#include "lua.h"

#include "dap_server.h"
#include "master_hook.h"

#define MAX_BREAKPOINTS  256
#define MAX_FRAMES       64
#define MAX_VARS         64
#define MAX_SOURCE_PATH  1024
#define MAX_MSG          (64 * 1024)

/* ── State ─────────────────────────────────────────────────────────────────── */

typedef struct {
    char source[MAX_SOURCE_PATH];
    int  line;
    int  id;
} wdap_bp_t;

static struct {
    int    connected;
    int    configuration_done;
    int    seq;

    wdap_bp_t bps[MAX_BREAKPOINTS];
    int       n_bps;
    int       next_bp_id;

    lua_State *paused_L;
    int        paused;
    int        continue_pending;

    int        pending_step_mode;
    int        pending_step_base_depth;
    int        pending_pause;
    int        hook_yielded;  /* set by fc_dap_pause_loop; cleared by fc_dap_hook_yielded */
} g_wdap;

/* ── JSON helpers ──────────────────────────────────────────────────────────── */

static int json_get_int(const char *buf, const char *key, int def) {
    char k[64];
    snprintf(k, sizeof k, "\"%s\"", key);
    const char *p = strstr(buf, k);
    if (!p) return def;
    p = strchr(p, ':');
    if (!p) return def;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    return atoi(p);
}

static int json_get_string(const char *buf, const char *key, char *out, size_t n) {
    char k[64];
    snprintf(k, sizeof k, "\"%s\"", key);
    const char *p = strstr(buf, k);
    if (!p) return 0;
    p = strchr(p, ':');
    if (!p) return 0;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return 0;
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
    if (!p) return 0;
    p = strchr(p, '[');
    if (!p) return 0;
    p++;
    int n = 0;
    while (*p && *p != ']' && n < max) {
        while (*p == ' ' || *p == ',' || *p == '\t' || *p == '\n') p++;
        if (*p == ']') break;
        out[n++] = atoi(p);
        while (*p && *p != ',' && *p != ']') p++;
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

static void send_response(int request_seq, const char *command,
                          int success, const char *body_or_msg) {
    static char buf[MAX_MSG];
    int  seq = ++g_wdap.seq;
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
    int  seq = ++g_wdap.seq;
    snprintf(buf, sizeof buf,
             "{\"seq\":%d,\"type\":\"event\",\"event\":\"%s\",\"body\":%s}",
             seq, event, body ? body : "{}");
    send_json(buf);
}

/* ── Request handlers ──────────────────────────────────────────────────────── */

static void handle_initialize(int seq, const char *args) {
    (void)args;
    const char *body =
        "{\"supportsConfigurationDoneRequest\":true,"
        "\"supportsLoadedSourcesRequest\":true,"
        "\"supportsRestartRequest\":false,"
        "\"supportsStepBack\":false,"
        "\"supportsTerminateRequest\":true}";
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

/* Parse "breakpoints":[{"line":N},{...}] — preferred VS Code format. */
static int json_get_bp_array_lines(const char *buf, int *out, int max) {
    const char *p = strstr(buf, "\"breakpoints\"");
    if (!p) return 0;
    p = strchr(p, '[');
    if (!p) return 0;
    const char *end = strchr(p, ']');
    if (!end) return 0;
    int n = 0;
    p++;
    while (p < end && n < max) {
        const char *lp = strstr(p, "\"line\"");
        if (!lp || lp >= end) break;
        lp = strchr(lp, ':');
        if (!lp || lp >= end) break;
        lp++;
        while (*lp == ' ' || *lp == '\t') lp++;
        out[n++] = atoi(lp);
        p = lp;
    }
    return n;
}

static void handle_set_breakpoints(int seq, const char *args) {
    static char source[MAX_SOURCE_PATH];
    source[0] = '\0';
    json_get_string(args, "path", source, sizeof source);

    int lines[MAX_BREAKPOINTS];
    int n = json_get_bp_array_lines(args, lines, MAX_BREAKPOINTS);
    if (n == 0)
        n = json_get_int_array(args, "lines", lines, MAX_BREAKPOINTS);

    /* Replace all breakpoints for this source. */
    int kept = 0;
    for (int i = 0; i < g_wdap.n_bps; i++) {
        if (strcmp(g_wdap.bps[i].source, source) != 0) {
            if (kept != i) g_wdap.bps[kept] = g_wdap.bps[i];
            kept++;
        }
    }
    g_wdap.n_bps = kept;
    for (int i = 0; i < n && g_wdap.n_bps < MAX_BREAKPOINTS; i++) {
        wdap_bp_t *bp = &g_wdap.bps[g_wdap.n_bps++];
        snprintf(bp->source, sizeof bp->source, "%s", source);
        bp->line = lines[i];
        bp->id   = ++g_wdap.next_bp_id;
    }

    static char body[MAX_MSG];
    int off = snprintf(body, sizeof body, "{\"breakpoints\":[");
    for (int i = 0; i < n; i++) {
        int fid = 0;
        for (int j = 0; j < g_wdap.n_bps; j++) {
            if (strcmp(g_wdap.bps[j].source, source) == 0 &&
                g_wdap.bps[j].line == lines[i]) {
                fid = g_wdap.bps[j].id;
                break;
            }
        }
        off += snprintf(body + off, sizeof(body) - (size_t)off,
                        "%s{\"id\":%d,\"verified\":true,\"line\":%d}",
                        i ? "," : "", fid, lines[i]);
    }
    snprintf(body + off, sizeof(body) - (size_t)off, "]}");
    send_response(seq, "setBreakpoints", 1, body);
}

static void handle_threads(int seq, const char *args) {
    (void)args;
    send_response(seq, "threads", 1,
                  "{\"threads\":[{\"id\":1,\"name\":\"cart\"}]}");
}

static void handle_stack_trace(int seq, const char *args) {
    (void)args;
    lua_State *L = g_wdap.paused_L;
    if (!L) {
        send_response(seq, "stackTrace", 0, "not paused");
        return;
    }
    static char body[MAX_MSG];
    int  off         = snprintf(body, sizeof body, "{\"stackFrames\":[");
    int  display     = 0;
    int  lua_frame   = 0;
    lua_Debug ar;
    while (lua_frame < MAX_FRAMES && lua_getstack(L, lua_frame, &ar)) {
        lua_getinfo(L, "Snl", &ar);
        lua_frame++;
        /* Skip synthetic frames (inline strings, C functions).
         * Only show @-prefixed file-backed sources so VS Code gets real paths. */
        const char *src = ar.source ? ar.source : "";
        if (*src != '@') continue;
        src++;  /* strip leading @ */
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

static void handle_scopes(int seq, const char *args) {
    int frame_id = json_get_int(args, "frameId", 0);
    char body[256];
    snprintf(body, sizeof body,
             "{\"scopes\":[{\"name\":\"Locals\",\"variablesReference\":%d,"
             "\"expensive\":false}]}",
             frame_id + 1);
    send_response(seq, "scopes", 1, body);
}

static int append_variable(char *buf, size_t remaining, const char *vn,
                           lua_State *L, int first) {
    const char *vt = luaL_typename(L, -1);
    char val[256]  = {0};
    if (lua_isstring(L, -1) || lua_isnumber(L, -1)) {
        const char *s = lua_tostring(L, -1);
        snprintf(val, sizeof val, "%s", s ? s : "?");
    } else {
        snprintf(val, sizeof val, "%s", vt);
    }
    for (char *q = val; *q; q++)
        if (*q == '"' || *q == '\\') *q = '_';
    return snprintf(buf, remaining,
                    "%s{\"name\":\"%s\",\"value\":\"%s\","
                    "\"type\":\"%s\",\"variablesReference\":0}",
                    first ? "" : ",", vn, val, vt);
}

static void handle_variables(int seq, const char *args) {
    int vref     = json_get_int(args, "variablesReference", 0);
    int frame_id = vref - 1;
    lua_State *L = g_wdap.paused_L;
    if (!L || frame_id < 0) {
        send_response(seq, "variables", 1, "{\"variables\":[]}");
        return;
    }
    static char body[MAX_MSG];
    int  off = snprintf(body, sizeof body, "{\"variables\":[");
    lua_Debug ar;
    if (lua_getstack(L, frame_id, &ar)) {
        int first = 1;

        /* Local variables. */
        const char *vn;
        int idx = 1;
        while ((vn = lua_getlocal(L, &ar, idx)) != NULL && idx <= MAX_VARS) {
            if (vn[0] != '(') {
                off += append_variable(body + off, sizeof(body) - (size_t)off,
                                       vn, L, first);
                first = 0;
            }
            lua_pop(L, 1);
            idx++;
        }

        /* Upvalues of the current function (captures like outer locals). */
        lua_getinfo(L, "f", &ar);  /* pushes function onto stack */
        int fn  = lua_gettop(L);
        int uvi = 1;
        while (uvi <= MAX_VARS) {
            vn = lua_getupvalue(L, fn, uvi++);
            if (!vn) break;
            if (strcmp(vn, "_ENV") != 0) {
                off += append_variable(body + off, sizeof(body) - (size_t)off,
                                       vn, L, first);
                first = 0;
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);  /* pop function */
    }
    snprintf(body + off, sizeof(body) - (size_t)off, "]}");
    send_response(seq, "variables", 1, body);
}

static void handle_evaluate(int seq, const char *args) {
    char expr[256] = {0};
    json_get_string(args, "expression", expr, sizeof expr);
    int frame_id = json_get_int(args, "frameId", 0);

    lua_State *L = g_wdap.paused_L;
    if (!L || expr[0] == '\0') {
        send_response(seq, "evaluate", 1, "{\"result\":\"?\",\"variablesReference\":0}");
        return;
    }

    lua_Debug ar;
    if (!lua_getstack(L, frame_id, &ar)) {
        send_response(seq, "evaluate", 1, "{\"result\":\"?\",\"variablesReference\":0}");
        return;
    }

    /* Search locals then upvalues for a variable matching expr. */
    const char *vn;
    int idx = 1;
    while ((vn = lua_getlocal(L, &ar, idx++)) != NULL) {
        if (strcmp(vn, expr) == 0) goto found;
        lua_pop(L, 1);
    }
    lua_getinfo(L, "f", &ar);
    {
        int fn = lua_gettop(L), uvi = 1;
        while ((vn = lua_getupvalue(L, fn, uvi++)) != NULL) {
            if (strcmp(vn, expr) == 0) { lua_remove(L, fn); goto found; }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);  /* pop function */
    }
    send_response(seq, "evaluate", 1, "{\"result\":\"?\",\"variablesReference\":0}");
    return;

found: {
        static char body[MAX_MSG];
        char val[256] = {0};
        if (lua_isstring(L, -1) || lua_isnumber(L, -1)) {
            const char *s = lua_tostring(L, -1);
            snprintf(val, sizeof val, "%s", s ? s : "?");
        } else {
            snprintf(val, sizeof val, "%s", luaL_typename(L, -1));
        }
        lua_pop(L, 1);
        for (char *q = val; *q; q++)
            if (*q == '"' || *q == '\\') *q = '_';
        snprintf(body, sizeof body,
                 "{\"result\":\"%s\",\"variablesReference\":0}", val);
        send_response(seq, "evaluate", 1, body);
    }
}

static void handle_continue(int seq, const char *args) {
    (void)args;
    g_wdap.continue_pending  = 1;
    g_wdap.pending_step_mode = 0;
    g_wdap.pending_pause     = 0;
    send_response(seq, "continue", 1, "{\"allThreadsContinued\":true}");
}

static void handle_step(int seq, const char *args, int mode) {
    (void)args;
    /* Depth at pause time — used for step-over / step-out depth comparison. */
    int depth = 0;
    if (g_wdap.paused_L) {
        lua_Debug ar;
        while (lua_getstack(g_wdap.paused_L, depth, &ar)) depth++;
    }
    g_wdap.pending_step_mode       = mode;
    g_wdap.pending_step_base_depth = depth;
    g_wdap.pending_pause           = 0;
    g_wdap.continue_pending        = 1;
    const char *cmd = (mode == DAP_STEP_OVER) ? "next"
                    : (mode == DAP_STEP_IN)   ? "stepIn"
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
    g_wdap.continue_pending  = 1;
    g_wdap.pending_step_mode = 0;
    g_wdap.pending_pause     = 0;
}

static void handle_source(int seq, const char *args) {
    (void)args;
    send_response(seq, "source", 0, "source not available");
}

/* ── Dispatcher ────────────────────────────────────────────────────────────── */

static void dispatch(const char *msg) {
    int  seq = json_get_int(msg, "seq", 0);
    char cmd[64];
    if (!json_get_string(msg, "command", cmd, sizeof cmd)) return;

    if      (strcmp(cmd, "initialize")        == 0) handle_initialize(seq, msg);
    else if (strcmp(cmd, "launch")            == 0) handle_launch(seq, msg);
    else if (strcmp(cmd, "configurationDone") == 0) handle_configuration_done(seq, msg);
    else if (strcmp(cmd, "setBreakpoints")    == 0) handle_set_breakpoints(seq, msg);
    else if (strcmp(cmd, "threads")           == 0) handle_threads(seq, msg);
    else if (strcmp(cmd, "stackTrace")        == 0) handle_stack_trace(seq, msg);
    else if (strcmp(cmd, "scopes")            == 0) handle_scopes(seq, msg);
    else if (strcmp(cmd, "variables")         == 0) handle_variables(seq, msg);
    else if (strcmp(cmd, "evaluate")          == 0) handle_evaluate(seq, msg);
    else if (strcmp(cmd, "continue")          == 0) handle_continue(seq, msg);
    else if (strcmp(cmd, "next")              == 0) handle_step(seq, msg, DAP_STEP_OVER);
    else if (strcmp(cmd, "stepIn")            == 0) handle_step(seq, msg, DAP_STEP_IN);
    else if (strcmp(cmd, "stepOut")           == 0) handle_step(seq, msg, DAP_STEP_OUT);
    else if (strcmp(cmd, "pause")             == 0) handle_pause(seq, msg);
    else if (strcmp(cmd, "disconnect")        == 0) handle_disconnect(seq, msg);
    else if (strcmp(cmd, "terminate")         == 0) handle_disconnect(seq, msg);
    else if (strcmp(cmd, "source")            == 0) handle_source(seq, msg);
    else send_response(seq, cmd, 0, "unknown command");
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
        if (!json) break;
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
            try { Module._wdap_ws.close(1000, 'shutdown'); } catch(e) {}
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
        if (*q == '/') b = q + 1;
    return b;
}

bool fc_dap_should_break(lua_State *L, lua_Debug *ar) {
    if (!wdap_is_connected_js()) return false;
    if (g_wdap.pending_pause) return true;
    lua_getinfo(L, "Sl", ar);
    const char *src = ar->source ? ar->source : "";
    if (*src == '@') src++;
    const char *src_base = basename_of(src);
    for (int i = 0; i < g_wdap.n_bps; i++) {
        if (g_wdap.bps[i].line != ar->currentline) continue;
        if (strcmp(basename_of(g_wdap.bps[i].source), src_base) == 0)
            return true;
    }
    return false;
}

void fc_dap_pause_loop(lua_State *L, lua_Debug *ar) {
    (void)ar;
    g_wdap.paused_L         = L;
    g_wdap.paused           = 1;
    g_wdap.continue_pending = 0;
    g_wdap.pending_pause    = 0;
    g_wdap.hook_yielded     = 1;  /* signal to wasm_lua_loop */
    { lua_Debug info; lua_getstack(L, 0, &info); lua_getinfo(L, "Sl", &info);
      printf("[dap] pause_loop: line=%d src=%s\n",
             info.currentline, info.source ? info.source : "?"); }

    const char *reason = (fc_master_hook_cfg.dap_pending_pause) ? "pause"
                       : (fc_master_hook_cfg.dap_step_mode != DAP_STEP_NONE) ? "step"
                       : "breakpoint";
    char body[256];
    snprintf(body, sizeof body,
             "{\"reason\":\"%s\",\"threadId\":1,\"allThreadsStopped\":true}",
             reason);
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
    printf("[dap] do_resume: step_mode=%d step_base=%d\n", step_mode, step_base);
    g_wdap.paused_L         = NULL;
    g_wdap.paused           = 0;
    g_wdap.continue_pending = 0;

    fc_master_hook_cfg.dap_step_mode       = (dap_step_mode_t)step_mode;
    fc_master_hook_cfg.dap_step_base_depth = step_base;
    fc_master_hook_cfg.dap_pending_pause   = 0;

    send_event("continued", "{\"threadId\":1,\"allThreadsContinued\":true}");
}

void fc_dap_output(const char *msg) {
    if (!wdap_is_connected_js()) return;
    static char body[MAX_MSG];
    char esc[MAX_MSG / 2];
    size_t j = 0;
    for (size_t i = 0; msg[i] && j + 4 < sizeof esc; i++) {
        if      (msg[i] == '"')  { esc[j++] = '\\'; esc[j++] = '"';  }
        else if (msg[i] == '\\') { esc[j++] = '\\'; esc[j++] = '\\'; }
        else if (msg[i] == '\n') { esc[j++] = '\\'; esc[j++] = 'n';  }
        else if (msg[i] == '\r') { esc[j++] = '\\'; esc[j++] = 'r';  }
        else                     { esc[j++] = msg[i]; }
    }
    esc[j] = '\0';
    snprintf(body, sizeof body,
             "{\"category\":\"stdout\",\"output\":\"%s\\n\"}", esc);
    send_event("output", body);
}

void fc_dap_emit_loaded_source(const char *source_path) {
    char body[MAX_SOURCE_PATH + 128];
    snprintf(body, sizeof body,
             "{\"reason\":\"changed\",\"source\":{\"path\":\"%s\",\"name\":\"%s\"}}",
             source_path, source_path);
    send_event("loadedSource", body);
}

/* ── ECALL stubs (RV32 ELF cart path — not used in WASM Lua builds) ───────── */

int fc_dap_check_hook_line(const char *source, int line, int depth) {
    (void)source; (void)line; (void)depth;
    return 0;
}

void fc_dap_host_send(const char *json, size_t len) {
    (void)json; (void)len;
}

int fc_dap_host_recv(char *buf, size_t max_len) {
    (void)buf; (void)max_len;
    return 0;
}
