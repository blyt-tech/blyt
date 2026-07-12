/* runtime/host/src/dap/dap_transport_tcp_lua.c
 *
 * Native TCP transport for the host-side Lua DAP debugger (issue #234).
 *
 * Serves the native host-Lua runner (cart_run_hostlua.c): a pure-Lua cart runs
 * in a host-native Lua VM, so the debugger inspects a REAL lua_State* directly —
 * the same shared inspection core (dap_lua_inspect.c) the WASM leg uses.  Unlike
 * the emulated RV32 path (dap_server.c), there is no ECALL forwarding to a guest;
 * and unlike WASM, the host is multi-threaded, so pausing BLOCKS the execution
 * thread instead of yielding a coroutine:
 *
 *   - A reader pthread accepts one client and reads Content-Length-framed DAP
 *     messages.  While the cart is RUNNING it dispatches each message inline
 *     (breakpoints / launch / configurationDone — none touch the live VM).
 *   - When a line hook hits a breakpoint, fc_dap_pause_loop() BLOCKS the
 *     execution thread (the frontend's retro_run) in a service loop.  While
 *     paused, the reader thread only enqueues messages; the blocked execution
 *     thread drains the queue and runs every handler (stackTrace / variables /
 *     evaluate) itself, so ALL lua_State access stays on the one thread that
 *     owns the VM.  It returns when the client sends continue / step / disconnect.
 *
 * One mutex serialises every entry into the shared core (the reader thread's
 * inline dispatch vs. the execution thread's hook checks + paused-service
 * dispatch), which the core requires (it keeps no lock and uses static buffers).
 *
 * The distinct fc_hostlua_dap_* namespace lets this TU link alongside
 * dap_server.c in blytdebug; it additionally provides the strong master-hook
 * callbacks fc_dap_should_break / fc_dap_pause_loop (weak in master_hook.h).
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "lua.h"

#include "blyt_trace.h"
#include "dap_lua_inspect.h"
#include "dap_transport_tcp_lua.h"
#include "master_hook.h"

#define HLDAP_MAX_MSG (1 << 20)
#define HLDAP_QUEUE_CAP 64

/* ── State ─────────────────────────────────────────────────────────────────── */

static struct {
    pthread_mutex_t mu;
    pthread_cond_t cond; /* signalled by the reader thread on enqueue / config / disconnect */
    pthread_t thr;
    int listen_fd;
    int client_fd;
    int running;
    int client_gone; /* client disconnected while the exec thread was paused */

    /* Inspection-command queue drained by the paused execution thread. */
    char *queue[HLDAP_QUEUE_CAP];
    int q_head, q_tail;
} g_hl = {.mu = PTHREAD_MUTEX_INITIALIZER, .listen_fd = -1, .client_fd = -1};

/* ── Wire I/O ──────────────────────────────────────────────────────────────── */

static int write_all(int fd, const char *p, size_t n) {
    while (n) {
#ifdef MSG_NOSIGNAL
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
#else
        ssize_t w = send(fd, p, n, 0);
#endif
        if (w <= 0)
            return -1;
        p += (size_t)w;
        n -= (size_t)w;
    }
    return 0;
}

/* Transport op: the shared core hands us one complete DAP JSON message; frame it
 * with a Content-Length header and write it to the client.  Always called with
 * g_hl.mu held (from the reader thread's inline dispatch or the exec thread's
 * paused-service dispatch), so writes are already serialised. */
static void hldap_send(const char *json) {
    if (g_hl.client_fd < 0)
        return;
    blyt_tracef(BLYT_TRACE_DAP, "send %s", json);
    char hdr[64];
    int hn = snprintf(hdr, sizeof hdr, "Content-Length: %zu\r\n\r\n", strlen(json));
    if (write_all(g_hl.client_fd, hdr, (size_t)hn) == 0)
        write_all(g_hl.client_fd, json, strlen(json));
}

static int hldap_is_connected(void) {
    return g_hl.client_fd >= 0;
}

static const dap_lua_transport_ops_t hldap_ops = {
    .send = hldap_send,
    .is_connected = hldap_is_connected,
};

/* Read one Content-Length-framed DAP message from fd into buf.  Returns the body
 * length (>0), or <=0 on EOF / error. */
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

/* ── Inspection-command queue (paused → execution thread) ──────────────────── */

/* Enqueue a copy of `msg` for the paused execution thread to dispatch; signal it.
 * Caller holds g_hl.mu.  Drops the message if the queue is full (a client that
 * floods a paused session; bounded so a stuck exec thread can't OOM us). */
static void queue_push(const char *msg) {
    int next = (g_hl.q_tail + 1) % HLDAP_QUEUE_CAP;
    if (next == g_hl.q_head) {
        blyt_tracef(BLYT_TRACE_DAP, "inspection queue full — dropping message");
        return;
    }
    g_hl.queue[g_hl.q_tail] = strdup(msg);
    if (!g_hl.queue[g_hl.q_tail])
        return;
    g_hl.q_tail = next;
    pthread_cond_signal(&g_hl.cond);
}

/* Pop the next queued message (caller frees), or NULL if empty.  Caller holds mu. */
static char *queue_pop(void) {
    if (g_hl.q_head == g_hl.q_tail)
        return NULL;
    char *m = g_hl.queue[g_hl.q_head];
    g_hl.q_head = (g_hl.q_head + 1) % HLDAP_QUEUE_CAP;
    return m;
}

static void queue_clear(void) {
    while (g_hl.q_head != g_hl.q_tail) {
        free(g_hl.queue[g_hl.q_head]);
        g_hl.q_head = (g_hl.q_head + 1) % HLDAP_QUEUE_CAP;
    }
}

/* ── Reader thread ─────────────────────────────────────────────────────────── */

static void *reader_thread(void *arg) {
    (void)arg;
    static char buf[HLDAP_MAX_MSG];
    while (g_hl.running) {
        struct pollfd pfd = {.fd = g_hl.listen_fd, .events = POLLIN};
        if (poll(&pfd, 1, 100) <= 0)
            continue;
        struct sockaddr_in cli;
        socklen_t cl = sizeof cli;
        int fd = accept(g_hl.listen_fd, (struct sockaddr *)&cli, &cl);
        if (fd < 0) {
            if (!g_hl.running)
                break;
            continue;
        }
        pthread_mutex_lock(&g_hl.mu);
        g_hl.client_fd = fd;
        g_hl.client_gone = 0;
        pthread_mutex_unlock(&g_hl.mu);
        blyt_tracef(BLYT_TRACE_DAP, "client connected");

        while (g_hl.running) {
            int n = read_msg(fd, buf, sizeof buf);
            if (n <= 0)
                break;
            blyt_tracef(BLYT_TRACE_DAP, "recv %s", buf);
            pthread_mutex_lock(&g_hl.mu);
            if (dap_lua_is_paused()) {
                /* Paused (breakpoint/step OR an exception stop): the execution
                 * thread owns the VM, so it must run every handler.  Enqueue for
                 * it to drain in-order.  Gating on dap_lua_is_paused (not
                 * dap_lua_paused_state) matters for an exception stop, which
                 * parks with paused_L == NULL yet still needs its requests
                 * serviced by the blocked exec thread. */
                queue_push(buf);
            } else {
                /* Running: no live VM to touch — dispatch inline and wake any
                 * thread blocked in fc_hostlua_dap_wait_ready on configurationDone. */
                dap_lua_dispatch(buf);
                pthread_cond_broadcast(&g_hl.cond);
            }
            pthread_mutex_unlock(&g_hl.mu);
        }

        blyt_tracef(BLYT_TRACE_DAP, "client disconnected");
        pthread_mutex_lock(&g_hl.mu);
        close(fd);
        g_hl.client_fd = -1;
        g_hl.client_gone = 1;
        /* Release a paused execution thread + any config waiter. */
        pthread_cond_broadcast(&g_hl.cond);
        pthread_mutex_unlock(&g_hl.mu);
    }
    return NULL;
}

/* ── Master-hook callbacks (native host-Lua path) ──────────────────────────── */

bool fc_dap_should_break(lua_State *L, lua_Debug *ar) {
    pthread_mutex_lock(&g_hl.mu);
    bool r = dap_lua_should_break(L, ar);
    pthread_mutex_unlock(&g_hl.mu);
    return r;
}

/* Block the execution thread at a breakpoint / step stop.  Emits "stopped",
 * then services inspection requests (on THIS thread, the VM owner) until the
 * client resumes or disconnects. */
void fc_dap_pause_loop(lua_State *L, lua_Debug *ar) {
    (void)ar;
    pthread_mutex_lock(&g_hl.mu);
    dap_lua_enter_paused(L); /* sets paused_L + emits "stopped" via hldap_send */

    while (g_hl.running && !g_hl.client_gone && !dap_lua_continue_pending()) {
        char *msg = queue_pop();
        if (msg) {
            dap_lua_dispatch(msg); /* stackTrace / variables / evaluate against L */
            free(msg);
        } else {
            pthread_cond_wait(&g_hl.cond, &g_hl.mu);
        }
    }

    dap_lua_apply_resume(); /* clears paused_L, applies step mode, emits "continued" */
    queue_clear(); /* drop any unserviced requests from this pause */
    pthread_mutex_unlock(&g_hl.mu);
}

/* Test-and-clear the pending-restart flag (issue #257).  The host-Lua run loop
 * polls this at frame entry and, when set, returns BLYT_RUN_RESTART so the
 * frontend rebuilds the VM and re-waits for configurationDone — the native
 * analog of the emulated path's BLYT_RUN_RESTART + blyt_session_dap_reattach.
 * Mutex-guarded because the flag lives in the shared core, which the reader
 * thread may be mutating concurrently. */
int fc_hostlua_dap_restart_pending(void) {
    pthread_mutex_lock(&g_hl.mu);
    int v = dap_lua_restart_pending();
    pthread_mutex_unlock(&g_hl.mu);
    return v;
}

/* Report a Lua error to the DAP client and, if an exception breakpoint filter
 * matches, BLOCK the execution thread on the exception stop until the client
 * resumes / disconnects (issue #257).  The native analog of the WASM leg's
 * blyt_dap_report_exception → g_lua_dap_paused park: dap_lua_on_exception emits
 * "stopped" (reason=exception, paused_L == NULL — the error unwound the frame),
 * then this services inspection requests on THIS thread (the VM owner) exactly
 * as fc_dap_pause_loop does for a breakpoint stop.  Returns 1 if it paused, 0 if
 * no filter matched (the caller then handles the error as a plain, non-paused
 * Lua error).  Called from the runner's call_lifecycle on a lua_pcall failure. */
int fc_hostlua_dap_report_exception(const char *msg, int is_uncaught) {
    pthread_mutex_lock(&g_hl.mu);
    if (!dap_lua_on_exception(msg, is_uncaught)) {
        pthread_mutex_unlock(&g_hl.mu);
        return 0;
    }
    while (g_hl.running && !g_hl.client_gone && !dap_lua_continue_pending()) {
        char *m = queue_pop();
        if (m) {
            dap_lua_dispatch(m);
            free(m);
        } else {
            pthread_cond_wait(&g_hl.cond, &g_hl.mu);
        }
    }
    dap_lua_apply_resume(); /* clears the paused state, emits "continued" */
    queue_clear();
    pthread_mutex_unlock(&g_hl.mu);
    return 1;
}

/* ── Public API ────────────────────────────────────────────────────────────── */

int fc_hostlua_dap_listen(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
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

    pthread_cond_init(&g_hl.cond, NULL);
    dap_lua_set_transport(&hldap_ops);
    dap_lua_reset();
    g_hl.q_head = g_hl.q_tail = 0;
    g_hl.client_gone = 0;
    g_hl.listen_fd = fd;
    g_hl.client_fd = -1;
    g_hl.running = 1;
    if (pthread_create(&g_hl.thr, NULL, reader_thread, NULL) != 0) {
        close(fd);
        g_hl.listen_fd = -1;
        g_hl.running = 0;
        return -1;
    }
    return actual_port;
}

int fc_hostlua_dap_wait_ready(void) {
    pthread_mutex_lock(&g_hl.mu);
    while (g_hl.running && !dap_lua_configuration_done())
        pthread_cond_wait(&g_hl.cond, &g_hl.mu);
    int done = dap_lua_configuration_done();
    pthread_mutex_unlock(&g_hl.mu);
    return done;
}

void fc_hostlua_dap_shutdown(void) {
    if (!g_hl.running)
        return;
    g_hl.running = 0;
    /* Release a paused execution thread. */
    pthread_mutex_lock(&g_hl.mu);
    pthread_cond_broadcast(&g_hl.cond);
    pthread_mutex_unlock(&g_hl.mu);
    if (g_hl.client_fd >= 0)
        shutdown(g_hl.client_fd, SHUT_RDWR);
    int lfd = g_hl.listen_fd;
    if (lfd >= 0) {
        shutdown(lfd, SHUT_RDWR);
        close(lfd);
        g_hl.listen_fd = -1;
    }
    pthread_join(g_hl.thr, NULL);
    pthread_mutex_lock(&g_hl.mu);
    if (g_hl.client_fd >= 0) {
        close(g_hl.client_fd);
        g_hl.client_fd = -1;
    }
    queue_clear();
    pthread_mutex_unlock(&g_hl.mu);
}
