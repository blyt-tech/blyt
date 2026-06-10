/* runtime/guest/src/libblyt32lua/master_hook_native.c
 *
 * Strong definitions of blyt_dap_active / fc_dap_should_break /
 * fc_dap_pause_loop / fc_dap_wait_ready_for_cart for the native QEMU path.
 *
 * On this path the cart runs as a real ILP32 process inside a QEMU VM; there
 * is no rv32emu host to service ECALLs.  dap_server.c is compiled directly
 * into the native libblyt32lua.so so all DAP logic lives in the cart process.
 *
 * Start-up sequence in blyt_cart_init():
 *   1. blyt_dap_active() reads BLYT_DAP_PORT, calls fc_consolelua_dap_listen(),
 *      prints "blyt: DAP listening on port N" to stderr, returns 1.
 *   2. fc_consolelua_master_hook_install() arms the Lua line hook.
 *   3. fc_dap_wait_ready_for_cart() blocks until the DAP client sends
 *      configurationDone (so breakpoints are set before init() runs).
 *
 * Inspection loop (fc_dap_pause_loop):
 *   Mirrors master_hook_ecall.c but calls fc_dap_host_send/recv directly
 *   instead of using BLYT_ECALL_DAP_* system calls.
 */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lauxlib.h"
#include "lua.h"

#include "blyt.h"
/* clang-format off */
/* master_hook.h must precede dap_server.h: it defines dap_step_mode_t enum
 * whose enumerators (DAP_STEP_OVER etc.) clash with same-named macros in
 * dap_server.h — the enum must be visible before the macros are defined. */
#include "master_hook.h"
#include "dap_server.h"
/* clang-format on */

#define MAX_FRAMES 32
#define MAX_VARS 64
#define MAX_SOURCE 512
#define MSG_MAX (32 * 1024)

/* ── JSON helpers (identical to master_hook_ecall.c) ──────────────────────── */

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
        } else {
            out[i++] = *p++;
        }
    }
    out[i] = 0;
    return 1;
}

/* ── BLYT_TRACE dap channel (native path) ─────────────────────────────────
 *
 * Self-contained mirror of the host trace module's dap channel: the host
 * trace code cannot run inside the ILP32 cart process, so the env check and
 * emission live here.  Lines use the host format minus frame/time counters
 * and go straight to fd 2 via a raw SYS_write (seccomp-allowlisted) —
 * deliberately not blyt_console_debug, whose own api-channel trace line
 * would echo every dap line a second time. */
static void mh_trace(const char *dir, const char *msg) {
    static int enabled = -1;
    if (enabled < 0) {
        enabled = 0;
        const char *p = getenv("BLYT_TRACE");
        while (p && *p) {
            if (((p[0] == 'd' && p[1] == 'a' && p[2] == 'p') ||
                 (p[0] == 'a' && p[1] == 'l' && p[2] == 'l')) &&
                (p[3] == '\0' || p[3] == ',')) {
                enabled = 1;
                break;
            }
            while (*p && *p != ',')
                p++;
            if (*p)
                p++;
        }
    }
    if (!enabled)
        return;
    static char line[1024];
    int n = snprintf(line, sizeof line, "[blyt:dap] %s %.900s\n", dir, msg);
    if (n <= 0)
        return;
    if ((size_t)n >= sizeof line)
        n = (int)sizeof(line) - 1;
    register long a0 __asm__("a0") = 2; /* STDERR_FILENO */
    register const char *a1 __asm__("a1") = line;
    register long a2 __asm__("a2") = n;
    register long a7 __asm__("a7") = 64; /* SYS_write */
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
}

/* ── Response helpers ─────────────────────────────────────────────────────── */

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
    mh_trace("send", g_out);
    fc_dap_host_send(g_out, strlen(g_out));
}

/* ── Variable appender ────────────────────────────────────────────────────── */

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

/* ── Request handlers ─────────────────────────────────────────────────────── */

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

    const char *vn;
    int idx = 1;
    while ((vn = lua_getlocal(L, &ar2, idx++)) != NULL) {
        if (strcmp(vn, expr) == 0)
            goto found;
        lua_pop(L, 1);
    }
    lua_getinfo(L, "f", &ar2);
    {
        int fn = lua_gettop(L), uvi = 1;
        while ((vn = lua_getupvalue(L, fn, uvi++)) != NULL) {
            if (strcmp(vn, expr) == 0) {
                lua_remove(L, fn);
                goto found;
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    send_resp(seq, "evaluate", 1, "{\"result\":\"?\",\"variablesReference\":0}");
    return;

found: {
    char val[256] = {0};
    if (lua_isstring(L, -1) || lua_isnumber(L, -1))
        snprintf(val, sizeof val, "%s", lua_tostring(L, -1));
    else
        snprintf(val, sizeof val, "%s", luaL_typename(L, -1));
    lua_pop(L, 1);
    for (char *q = val; *q; q++)
        if (*q == '"' || *q == '\\')
            *q = '_';
    char body[320];
    snprintf(body, sizeof body, "{\"result\":\"%s\",\"variablesReference\":0}", val);
    send_resp(seq, "evaluate", 1, body);
}
}

static void on_threads(int seq) {
    send_resp(seq, "threads", 1, "{\"threads\":[{\"id\":1,\"name\":\"cart\"}]}");
}

/* ── Call depth ───────────────────────────────────────────────────────────── */

static int hook_call_depth(lua_State *L) {
    int depth = 0;
    lua_Debug ar;
    while (lua_getstack(L, depth, &ar))
        depth++;
    return depth;
}

/* ── Public callbacks ─────────────────────────────────────────────────────── */

/* Start the TCP DAP server if BLYT_DAP_PORT is set. */
int blyt_dap_active(void) {
    const char *port_str = getenv("BLYT_DAP_PORT");
    if (!port_str || !*port_str)
        return 0;
    int port = atoi(port_str);
    int actual = fc_consolelua_dap_listen(port);
    if (actual <= 0)
        return 0;
    char msg[64];
    snprintf(msg, sizeof msg, "blyt: DAP listening on port %d\n", actual);
    blyt_console_debug(msg);
    return 1;
}

bool fc_dap_should_break(lua_State *L, lua_Debug *ar) {
    lua_getinfo(L, "Sl", ar);
    const char *src = ar->source ? ar->source : "?";
    if (*src == '@')
        src++;
    int line = ar->currentline;
    int depth = hook_call_depth(L);
    return fc_dap_check_hook_line(src, line, depth) != 0;
}

/* Inspect loop: mirrors master_hook_ecall.c but uses direct function calls. */
void fc_dap_pause_loop(lua_State *L, lua_Debug *ar) {
    (void)ar;
    static char recv_buf[MSG_MAX];

    for (;;) {
        int len = fc_dap_host_recv(recv_buf, sizeof(recv_buf) - 1);
        if (len <= 0)
            break;

        recv_buf[len] = '\0';
        mh_trace("recv", recv_buf);
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

/* Block until configurationDone — called from blyt_cart_init after hook install. */
int fc_dap_wait_ready_for_cart(void) {
    return fc_dap_wait_configuration_done();
}
