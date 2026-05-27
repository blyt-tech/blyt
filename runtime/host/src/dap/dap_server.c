/* runtime/host/src/dap/dap_server.c
 *
 * DAP server — TCP+pthread transport for SDL2 and libretro targets.
 * Adapted from Spike J's dap_server.c for production use.
 *
 * The ECALL path (emulated Lua carts inside rv32emu):
 *   cart_run.c calls fc_dap_check_hook_line() on every BLYT_ECALL_DAP_HOOK.
 *   The host checks breakpoints + step state and, if the guest should pause,
 *   emits a "stopped" event and returns 1.  The guest then enters its own
 *   pause loop (master_hook_ecall.c::fc_dap_pause_loop), using
 *   BLYT_ECALL_DAP_SEND / BLYT_ECALL_DAP_RECV to forward inspection commands
 *   (stackTrace, variables, evaluate) to/from VS Code with direct lua_State*
 *   access inside the guest address space.  Continue/step/disconnect commands
 *   are handled on the host side and signal the RECV condvar.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "dap_server.h"

/* Step modes (must match dap_step_mode_t in master_hook.h). */
#define DAP_STEP_NONE 0
#define DAP_STEP_OVER 1
#define DAP_STEP_IN 2
#define DAP_STEP_OUT 3

#define MAX_BREAKPOINTS 256
#define MAX_SOURCE_PATH 1024
#define MAX_MSG (1 << 20)

/* Inspection-command queue delivered to the guest via BLYT_ECALL_DAP_RECV. */
#define MSG_QUEUE_CAP 8
#define MSG_BODY_MAX (32 * 1024)

typedef struct {
    char source[MAX_SOURCE_PATH];
    int line;
    int verified;
    int id;
} dap_bp_t;

typedef struct {
    pthread_mutex_t mu;
    pthread_t thr;
    int listen_fd;
    int client_fd;
    int running;
    int configuration_done;
    int seq;

    dap_bp_t bps[MAX_BREAKPOINTS];
    int n_bps;
    int next_bp_id;

    /* Pause state — populated by fc_dap_check_hook_line. */
    char paused_source[MAX_SOURCE_PATH];
    int paused_line;
    int paused_depth;
    int paused;
    int continue_pending; /* set when continue/step received during pause */

    /* Step command set by handle_step; read by fc_dap_check_hook_line. */
    int pending_step_mode;
    int pending_step_base_depth;
    int pending_pause;

    /* Guest-pause flag: 1 while the guest is inside fc_dap_pause_loop.
     * Inspection commands are queued when this is set. */
    int guest_paused;

    /* Inspection-command queue for delivery via BLYT_ECALL_DAP_RECV. */
    char q_buf[MSG_QUEUE_CAP][MSG_BODY_MAX];
    int q_head, q_tail;
    pthread_cond_t msg_cond;
} dap_state_t;

static dap_state_t g_dap = {.mu = PTHREAD_MUTEX_INITIALIZER, .client_fd = -1};

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

/* ── Wire I/O ──────────────────────────────────────────────────────────────── */

static int write_all(int fd, const char *p, size_t n) {
    while (n) {
        /* MSG_NOSIGNAL: return EPIPE instead of raising SIGPIPE when the peer
         * closes the connection.  Not available on macOS; SO_NOSIGPIPE handles
         * that side — see fc_consolelua_dap_listen(). */
#ifdef MSG_NOSIGNAL
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
#else
        ssize_t w = send(fd, p, n, 0);
#endif
        if (w <= 0)
            return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static void send_msg(const char *json) {
    if (g_dap.client_fd < 0)
        return;
    char hdr[64];
    int hn = snprintf(hdr, sizeof hdr, "Content-Length: %zu\r\n\r\n", strlen(json));
    pthread_mutex_lock(&g_dap.mu);
    if (write_all(g_dap.client_fd, hdr, (size_t)hn) == 0)
        write_all(g_dap.client_fd, json, strlen(json));
    pthread_mutex_unlock(&g_dap.mu);
}

static void send_response(int request_seq, const char *command, int success,
                          const char *body_or_msg) {
    pthread_mutex_lock(&g_dap.mu);
    int seq = ++g_dap.seq;
    pthread_mutex_unlock(&g_dap.mu);
    static char buf[MAX_MSG]; /* static: avoid 1 MB stack alloc on 512 KB thread */
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
    send_msg(buf);
}

static void send_event(const char *event, const char *body) {
    pthread_mutex_lock(&g_dap.mu);
    int seq = ++g_dap.seq;
    pthread_mutex_unlock(&g_dap.mu);
    static char buf[MAX_MSG]; /* static: avoid 1 MB stack alloc on 512 KB thread */
    snprintf(buf, sizeof buf, "{\"seq\":%d,\"type\":\"event\",\"event\":\"%s\",\"body\":%s}", seq,
             event, body ? body : "{}");
    send_msg(buf);
}

/* ── Inspection-command queue ──────────────────────────────────────────────── */

static int q_empty(void) {
    return g_dap.q_head == g_dap.q_tail;
}
static int q_full(void) {
    return ((g_dap.q_tail + 1) % MSG_QUEUE_CAP) == g_dap.q_head;
}

/* Push a raw DAP message JSON into the queue for the guest to process.
 * Must be called WITHOUT g_dap.mu held — takes and releases it internally. */
static void q_push(const char *msg) {
    pthread_mutex_lock(&g_dap.mu);
    if (!q_full()) {
        strncpy(g_dap.q_buf[g_dap.q_tail], msg, MSG_BODY_MAX - 1);
        g_dap.q_buf[g_dap.q_tail][MSG_BODY_MAX - 1] = '\0';
        g_dap.q_tail = (g_dap.q_tail + 1) % MSG_QUEUE_CAP;
        pthread_cond_signal(&g_dap.msg_cond);
    }
    pthread_mutex_unlock(&g_dap.mu);
}

/* ── Request handlers ──────────────────────────────────────────────────────── */

static void handle_initialize(int seq, const char *args) {
    (void)args;
    const char *body = "{\"supportsConfigurationDoneRequest\":true,"
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
    pthread_mutex_lock(&g_dap.mu);
    g_dap.configuration_done = 1;
    /* Wake fc_dap_check_hook_line which may be blocking on the first hook call. */
    pthread_cond_broadcast(&g_dap.msg_cond);
    pthread_mutex_unlock(&g_dap.mu);
    send_response(seq, "configurationDone", 1, "{}");
}

int fc_dap_configuration_done(void) {
    pthread_mutex_lock(&g_dap.mu);
    int v = g_dap.configuration_done;
    pthread_mutex_unlock(&g_dap.mu);
    return v;
}

static int json_get_bp_array_lines(const char *buf, int *out, int max) {
    const char *p = strstr(buf, "\"breakpoints\"");
    if (!p)
        return 0;
    p = strchr(p, '[');
    if (!p)
        return 0;
    const char *end = strchr(p, ']');
    if (!end)
        return 0;
    int n = 0;
    p++;
    while (p < end && n < max) {
        const char *lp = strstr(p, "\"line\"");
        if (!lp || lp >= end)
            break;
        lp = strchr(lp, ':');
        if (!lp || lp >= end)
            break;
        lp++;
        while (*lp == ' ' || *lp == '\t')
            lp++;
        out[n++] = atoi(lp);
        p = lp;
    }
    return n;
}

static void handle_set_breakpoints(int seq, const char *args) {
    char source[MAX_SOURCE_PATH] = {0};
    json_get_string(args, "path", source, sizeof source);

    int lines[MAX_BREAKPOINTS];
    int n = json_get_bp_array_lines(args, lines, MAX_BREAKPOINTS);
    if (n == 0)
        n = json_get_int_array(args, "lines", lines, MAX_BREAKPOINTS);

    pthread_mutex_lock(&g_dap.mu);
    int kept = 0;
    for (int i = 0; i < g_dap.n_bps; i++) {
        if (strcmp(g_dap.bps[i].source, source) != 0) {
            if (kept != i)
                g_dap.bps[kept] = g_dap.bps[i];
            kept++;
        }
    }
    g_dap.n_bps = kept;
    for (int i = 0; i < n && g_dap.n_bps < MAX_BREAKPOINTS; i++) {
        dap_bp_t *bp = &g_dap.bps[g_dap.n_bps++];
        snprintf(bp->source, sizeof bp->source, "%s", source);
        bp->line = lines[i];
        bp->verified = (lines[i] > 0);
        bp->id = ++g_dap.next_bp_id;
    }
    pthread_mutex_unlock(&g_dap.mu);

    static char body[MAX_MSG]; /* static: avoid 1 MB stack alloc on 512 KB thread */
    int off = snprintf(body, sizeof body, "{\"breakpoints\":[");
    pthread_mutex_lock(&g_dap.mu);
    for (int i = 0; i < n; i++) {
        int fid = 0, fv = 0, fl = lines[i];
        for (int j = 0; j < g_dap.n_bps; j++) {
            if (strcmp(g_dap.bps[j].source, source) == 0 && g_dap.bps[j].line == lines[i]) {
                fid = g_dap.bps[j].id;
                fv = g_dap.bps[j].verified;
                break;
            }
        }
        off += snprintf(body + off, sizeof(body) - (size_t)off,
                        "%s{\"id\":%d,\"verified\":%s,\"line\":%d}", i ? "," : "", fid,
                        fv ? "true" : "false", fl);
    }
    pthread_mutex_unlock(&g_dap.mu);
    snprintf(body + off, sizeof(body) - (size_t)off, "]}");
    send_response(seq, "setBreakpoints", 1, body);
}

static void handle_threads(int seq, const char *args) {
    (void)args;
    send_response(seq, "threads", 1, "{\"threads\":[{\"id\":1,\"name\":\"cart\"}]}");
}

static void handle_continue(int seq, const char *args) {
    (void)args;
    pthread_mutex_lock(&g_dap.mu);
    g_dap.pending_step_mode = 0;
    g_dap.pending_pause = 0;
    if (g_dap.guest_paused) {
        g_dap.continue_pending = 1;
        pthread_cond_signal(&g_dap.msg_cond);
    }
    pthread_mutex_unlock(&g_dap.mu);
    send_response(seq, "continue", 1, "{\"allThreadsContinued\":true}");
    send_event("continued", "{\"threadId\":1,\"allThreadsContinued\":true}");
}

static void handle_step(int seq, const char *args, int mode) {
    (void)args;
    /* Send the response first so the client is in gdb.recv() before we signal
     * the condvar.  If we signal first, the main thread can hit the GDB
     * breakpoint and send T05 before the "next" response is delivered, causing
     * the client to miss T05 in a race on Linux. */
    const char *cmd = (mode == DAP_STEP_OVER) ? "next"
                      : (mode == DAP_STEP_IN) ? "stepIn"
                                              : "stepOut";
    send_response(seq, cmd, 1, "{\"allThreadsContinued\":true}");
    send_event("continued", "{\"threadId\":1,\"allThreadsContinued\":true}");
    pthread_mutex_lock(&g_dap.mu);
    g_dap.pending_step_base_depth = g_dap.paused_depth;
    g_dap.pending_step_mode = mode;
    g_dap.pending_pause = 0;
    if (g_dap.guest_paused) {
        g_dap.continue_pending = 1;
        pthread_cond_signal(&g_dap.msg_cond);
    }
    pthread_mutex_unlock(&g_dap.mu);
}

static void handle_pause(int seq, const char *args) {
    (void)args;
    pthread_mutex_lock(&g_dap.mu);
    g_dap.pending_pause = 1;
    pthread_mutex_unlock(&g_dap.mu);
    send_response(seq, "pause", 1, "{}");
}

static void handle_disconnect(int seq, const char *args) {
    (void)args;
    send_response(seq, "disconnect", 1, "{}");
    pthread_mutex_lock(&g_dap.mu);
    g_dap.continue_pending = 1;
    g_dap.pending_step_mode = 0;
    g_dap.pending_pause = 0;
    if (g_dap.guest_paused)
        pthread_cond_signal(&g_dap.msg_cond);
    pthread_mutex_unlock(&g_dap.mu);
}

/* ── Dispatcher ────────────────────────────────────────────────────────────── */

static void dispatch(const char *msg) {
    int seq = json_get_int(msg, "seq", 0);
    char cmd[64];
    if (!json_get_string(msg, "command", cmd, sizeof cmd))
        return;

    /* Inspection commands need direct Lua access — queue for the guest when
     * it is inside its pause loop; return empty responses otherwise. */
    if (strcmp(cmd, "stackTrace") == 0 || strcmp(cmd, "scopes") == 0 ||
        strcmp(cmd, "variables") == 0 || strcmp(cmd, "evaluate") == 0) {
        pthread_mutex_lock(&g_dap.mu);
        int gp = g_dap.guest_paused;
        pthread_mutex_unlock(&g_dap.mu);
        if (gp) {
            q_push(msg);
        } else {
            if (strcmp(cmd, "stackTrace") == 0)
                send_response(seq, "stackTrace", 1, "{\"stackFrames\":[],\"totalFrames\":0}");
            else if (strcmp(cmd, "scopes") == 0)
                send_response(seq, "scopes", 1, "{\"scopes\":[]}");
            else if (strcmp(cmd, "variables") == 0)
                send_response(seq, "variables", 1, "{\"variables\":[]}");
            else
                send_response(seq, "evaluate", 1, "{\"result\":\"?\",\"variablesReference\":0}");
        }
        return;
    }

    if (strcmp(cmd, "initialize") == 0)
        handle_initialize(seq, msg);
    else if (strcmp(cmd, "launch") == 0)
        handle_launch(seq, msg);
    else if (strcmp(cmd, "configurationDone") == 0)
        handle_configuration_done(seq, msg);
    else if (strcmp(cmd, "setBreakpoints") == 0)
        handle_set_breakpoints(seq, msg);
    else if (strcmp(cmd, "threads") == 0)
        handle_threads(seq, msg);
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
    else if (strcmp(cmd, "disconnect") == 0)
        handle_disconnect(seq, msg);
    else if (strcmp(cmd, "terminate") == 0)
        handle_disconnect(seq, msg);
    else
        send_response(seq, cmd, 0, "unknown command");
}

/* ── Server thread ─────────────────────────────────────────────────────────── */

static int read_msg(int fd, char *buf, size_t buf_size) {
    size_t hi = 0;
    while (hi + 4 <= buf_size) {
        ssize_t r = recv(fd, buf + hi, 1, 0);
        if (r <= 0)
            return -1;
        hi++;
        if (hi >= 4 && memcmp(buf + hi - 4, "\r\n\r\n", 4) == 0)
            break;
    }
    buf[hi] = 0;
    const char *cl = strstr(buf, "Content-Length:");
    if (!cl)
        return -1;
    int len = atoi(cl + 15);
    if (len < 0 || (size_t)len + 1 > buf_size)
        return -1;
    size_t got = 0;
    while ((int)got < len) {
        ssize_t r = recv(fd, buf + got, (size_t)len - got, 0);
        if (r <= 0)
            return -1;
        got += (size_t)r;
    }
    buf[len] = 0;
    return len;
}

static void *dap_thread_main(void *arg) {
    (void)arg;
    static char buf[MAX_MSG];
    while (g_dap.running) {
        struct sockaddr_in cli;
        socklen_t cl = sizeof cli;
        int fd = accept(g_dap.listen_fd, (struct sockaddr *)&cli, &cl);
        if (fd < 0) {
            if (!g_dap.running)
                break;
            continue;
        }
        pthread_mutex_lock(&g_dap.mu);
        g_dap.client_fd = fd;
        pthread_mutex_unlock(&g_dap.mu);
        while (g_dap.running) {
            int n = read_msg(fd, buf, sizeof buf);
            if (n <= 0)
                break;
            dispatch(buf);
        }
        pthread_mutex_lock(&g_dap.mu);
        close(fd);
        g_dap.client_fd = -1;
        /* Wake fc_dap_host_recv if the guest is blocked waiting for a message. */
        if (g_dap.guest_paused)
            pthread_cond_signal(&g_dap.msg_cond);
        pthread_mutex_unlock(&g_dap.mu);
    }
    return NULL;
}

/* ── Public API ────────────────────────────────────────────────────────────── */

int fc_consolelua_dap_listen(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    /* Prevent SIGPIPE when the client closes the connection.  Linux uses
     * MSG_NOSIGNAL per-send; macOS needs SO_NOSIGPIPE on the socket. */
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        close(fd);
        return -1;
    }
    socklen_t sl = sizeof addr;
    if (getsockname(fd, (struct sockaddr *)&addr, &sl) < 0) {
        close(fd);
        return -1;
    }
    int actual_port = (int)ntohs(addr.sin_port);
    if (listen(fd, 1) < 0) {
        close(fd);
        return -1;
    }
    pthread_cond_init(&g_dap.msg_cond, NULL);
    g_dap.q_head = 0;
    g_dap.q_tail = 0;
    g_dap.guest_paused = 0;
    g_dap.continue_pending = 0;
    g_dap.listen_fd = fd;
    g_dap.client_fd = -1;
    g_dap.running = 1;
    if (pthread_create(&g_dap.thr, NULL, dap_thread_main, NULL) != 0) {
        close(fd);
        g_dap.running = 0;
        return -1;
    }
    return actual_port;
}

void fc_consolelua_dap_shutdown(void) {
    if (!g_dap.running)
        return;
    g_dap.running = 0;
    /* Wake fc_dap_host_recv if the guest is blocked. */
    pthread_mutex_lock(&g_dap.mu);
    pthread_cond_signal(&g_dap.msg_cond);
    pthread_mutex_unlock(&g_dap.mu);
    /* Interrupt any blocked recv() on the accepted client socket. */
    if (g_dap.client_fd >= 0)
        shutdown(g_dap.client_fd, SHUT_RDWR);
    /* close() (not just shutdown()) is needed to interrupt accept() on Linux:
     * shutdown(listen_fd) does not wake a blocked accept() there. */
    int lfd = g_dap.listen_fd;
    g_dap.listen_fd = -1;
    if (lfd >= 0)
        close(lfd);
    pthread_join(g_dap.thr, NULL);
    /* client_fd is closed by the server thread when it exits. */
    pthread_cond_destroy(&g_dap.msg_cond);
}

/* ── Breakpoint check helper ───────────────────────────────────────────────── */

static const char *basename_of(const char *p) {
    const char *b = p;
    for (const char *q = p; *q; q++)
        if (*q == '/')
            b = q + 1;
    return b;
}

static int check_bp(const char *source, int line) {
    if (g_dap.client_fd < 0)
        return 0;
    const char *src_base = basename_of(source);
    for (int i = 0; i < g_dap.n_bps; i++) {
        if (!g_dap.bps[i].verified)
            continue;
        if (g_dap.bps[i].line != line)
            continue;
        if (strcmp(basename_of(g_dap.bps[i].source), src_base) == 0)
            return 1;
    }
    return 0;
}

/* ── ECALL path (emulated Lua inside rv32emu) ──────────────────────────────── */

int fc_dap_check_hook_line(const char *source, int line, int depth) {
    if (g_dap.client_fd < 0)
        return 0;
    /* On the first hook call, wait for the client to finish configuration
     * (setBreakpoints, configurationDone) before checking any breakpoints.
     * This prevents the cart from running past init() before breakpoints are set. */
    pthread_mutex_lock(&g_dap.mu);
    while (g_dap.running && g_dap.client_fd >= 0 && !g_dap.configuration_done)
        pthread_cond_wait(&g_dap.msg_cond, &g_dap.mu);
    int alive = g_dap.running && g_dap.client_fd >= 0;
    pthread_mutex_unlock(&g_dap.mu);
    if (!alive)
        return 0;

    pthread_mutex_lock(&g_dap.mu);
    int pending_pause = g_dap.pending_pause;
    int step_mode = g_dap.pending_step_mode;
    int step_base_depth = g_dap.pending_step_base_depth;
    pthread_mutex_unlock(&g_dap.mu);

    int should_break = 0;
    const char *reason = "breakpoint";

    if (pending_pause) {
        should_break = 1;
        reason = "pause";
    } else if (step_mode != 0) {
        switch (step_mode) {
        case DAP_STEP_IN:
            should_break = 1;
            break;
        case DAP_STEP_OVER:
            should_break = (depth <= step_base_depth);
            break;
        case DAP_STEP_OUT:
            should_break = (depth < step_base_depth);
            break;
        default:
            break;
        }
        if (should_break)
            reason = "step";
    } else {
        pthread_mutex_lock(&g_dap.mu);
        should_break = check_bp(source, line);
        pthread_mutex_unlock(&g_dap.mu);
    }

    if (!should_break)
        return 0;

    pthread_mutex_lock(&g_dap.mu);
    g_dap.pending_pause = 0;
    g_dap.pending_step_mode = 0;
    snprintf(g_dap.paused_source, sizeof g_dap.paused_source, "%s", source);
    g_dap.paused_line = line;
    g_dap.paused_depth = depth;
    g_dap.paused = 1;
    g_dap.guest_paused = 1;
    g_dap.continue_pending = 0;
    pthread_mutex_unlock(&g_dap.mu);

    char body[MAX_SOURCE_PATH + 128];
    snprintf(body, sizeof body, "{\"reason\":\"%s\",\"threadId\":1,\"allThreadsStopped\":true}",
             reason);
    send_event("stopped", body);
    return 1;
}

/* Called from BLYT_ECALL_DAP_SEND: forward guest-built JSON to VS Code. */
void fc_dap_host_send(const char *json, size_t len) {
    (void)len;
    send_msg(json);
}

/* Called from BLYT_ECALL_DAP_RECV: block until an inspection command arrives
 * or the guest should resume.  Returns message length, 0 to resume. */
int fc_dap_host_recv(char *buf, size_t max_len) {
    pthread_mutex_lock(&g_dap.mu);
    while (g_dap.running && g_dap.client_fd >= 0 && !g_dap.continue_pending && q_empty()) {
        pthread_cond_wait(&g_dap.msg_cond, &g_dap.mu);
    }
    if (!g_dap.running || g_dap.client_fd < 0 || g_dap.continue_pending) {
        g_dap.continue_pending = 0;
        g_dap.paused = 0;
        g_dap.guest_paused = 0;
        pthread_mutex_unlock(&g_dap.mu);
        return 0;
    }
    /* Dequeue one inspection command. */
    size_t n = strnlen(g_dap.q_buf[g_dap.q_head], MSG_BODY_MAX - 1);
    if (n >= max_len)
        n = max_len - 1;
    memcpy(buf, g_dap.q_buf[g_dap.q_head], n);
    buf[n] = '\0';
    g_dap.q_head = (g_dap.q_head + 1) % MSG_QUEUE_CAP;
    pthread_mutex_unlock(&g_dap.mu);
    return (int)n;
}

/* ── Wait for configurationDone ────────────────────────────────────────────── */

int fc_dap_wait_configuration_done(void) {
    pthread_mutex_lock(&g_dap.mu);
    while (g_dap.running && !g_dap.configuration_done)
        pthread_cond_wait(&g_dap.msg_cond, &g_dap.mu);
    int result = g_dap.running && g_dap.configuration_done;
    pthread_mutex_unlock(&g_dap.mu);
    return result;
}

/* ── Miscellaneous public API ──────────────────────────────────────────────── */

void fc_dap_output(const char *msg) {
    static char esc[MAX_MSG / 2]; /* static: avoid large stack alloc */
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
    static char body[MAX_MSG]; /* static: avoid large stack alloc */
    snprintf(body, sizeof body, "{\"category\":\"stdout\",\"output\":\"%s\\n\"}", esc);
    send_event("output", body);
}

void fc_dap_poll_messages(void) { /* TCP transport uses a dedicated thread */
}

void fc_dap_emit_loaded_source(const char *source_path) {
    char body[MAX_SOURCE_PATH + 128];
    snprintf(body, sizeof body,
             "{\"reason\":\"changed\",\"source\":{\"path\":\"%s\",\"name\":\"%s\"}}", source_path,
             source_path);
    send_event("loadedSource", body);
}
