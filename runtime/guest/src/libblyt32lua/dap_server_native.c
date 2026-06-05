/* runtime/guest/src/libblyt32lua/dap_server_native.c
 *
 * Single-threaded TCP DAP server for the native ILP32 QEMU path.
 *
 * ld-blyt.so.1 is a restricted musl interpreter that does NOT export libc
 * symbols (getenv, malloc, socket, pthread, ...) into the process symbol table.
 * This file provides everything the cart needs via raw RISC-V Linux ecall:
 *
 *   Constructor (runs before cart code):
 *     1. Reads /proc/self/environ → sets __environ so getenv() works.
 *     2. Maps 4 MB via mmap → initialises blytc_arena so malloc() works.
 *
 *   getenv()        — uses our __environ (replaces libc version)
 *   fc_consolelua_dap_listen        — socket/bind/listen
 *   fc_dap_wait_configuration_done  — accept + protocol loop until configDone
 *   fc_dap_check_hook_line          — breakpoint/step check, emits "stopped"
 *   fc_dap_host_send                — send JSON to VS Code
 *   fc_dap_host_recv                — recv one inspection command (blocking)
 *
 * No pthreads, no blocking I/O outside of DAP pause points.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dap_server.h"

/* ── Raw RISC-V Linux syscall wrapper ───────────────────────────────────────── */

static long __sc(long nr, long a0, long a1, long a2, long a3, long a4, long a5) {
    register long _a7 __asm__("a7") = nr;
    register long _a0 __asm__("a0") = a0;
    register long _a1 __asm__("a1") = a1;
    register long _a2 __asm__("a2") = a2;
    register long _a3 __asm__("a3") = a3;
    register long _a4 __asm__("a4") = a4;
    register long _a5 __asm__("a5") = a5;
    __asm__ volatile("ecall"
                     : "+r"(_a0)
                     : "r"(_a7), "r"(_a1), "r"(_a2), "r"(_a3), "r"(_a4), "r"(_a5)
                     : "memory");
    return _a0;
}

static int n_openat(int dfd, const char *path, int flags) {
    return (int)__sc(56, dfd, (long)path, flags, 0, 0, 0);
}
static int n_read(int fd, void *buf, int n) {
    return (int)__sc(63, fd, (long)buf, n, 0, 0, 0);
}
static int n_close(int fd) {
    return (int)__sc(57, fd, 0, 0, 0, 0, 0);
}
static int n_socket(int domain, int type, int protocol) {
    return (int)__sc(198, domain, type, protocol, 0, 0, 0);
}
static int n_bind(int fd, const void *addr, int addrlen) {
    return (int)__sc(200, fd, (long)addr, addrlen, 0, 0, 0);
}
static int n_listen(int fd, int backlog) {
    return (int)__sc(201, fd, backlog, 0, 0, 0, 0);
}
static int n_accept4(int fd, void *addr, int *alen, int flags) {
    return (int)__sc(242, fd, (long)addr, (long)alen, flags, 0, 0);
}
static int n_getsockname(int fd, void *addr, int *alen) {
    return (int)__sc(204, fd, (long)addr, (long)alen, 0, 0, 0);
}
static int n_setsockopt(int fd, int lvl, int opt, const void *val, int len) {
    return (int)__sc(208, fd, lvl, opt, (long)val, len, 0);
}
static int n_send(int fd, const void *buf, size_t n) {
    return (int)__sc(206, fd, (long)buf, (long)n, 0, 0, 0);
}
static int n_recv_byte(int fd, void *b) {
    return (int)__sc(207, fd, (long)b, 1, 0, 0, 0);
}
static int n_recv_all(int fd, void *buf, int n) {
    char *p = (char *)buf;
    int got = 0;
    while (got < n) {
        long r = __sc(207, fd, (long)(p + got), n - got, 0, 0, 0);
        if (r <= 0)
            return -1;
        got += (int)r;
    }
    return got;
}
static void *n_mmap(size_t len) {
    /* PROT_READ|PROT_WRITE=3, MAP_PRIVATE|MAP_ANONYMOUS=0x22, fd=-1, off=0 */
    return (void *)__sc(222, 0, (long)len, 3, 0x22, -1, 0);
}

/* Network byte order (RISC-V is little-endian) */
static unsigned short n_htons(unsigned short x) {
    return (unsigned short)((x >> 8) | ((x & 0xFFu) << 8));
}

/* Minimal sockaddr_in without system socket headers */
typedef struct {
    unsigned short sin_family;
    unsigned short sin_port;
    unsigned int sin_addr;
    char sin_zero[8];
} n_saddr_t;

/* ── Environment + getenv ───────────────────────────────────────────────────── */

static char s_env_buf[64 * 1024];
static char *s_env_ptrs[1024];
char **__environ;

char *getenv(const char *name) {
    if (!name || !__environ)
        return NULL;
    size_t l = 0;
    while (name[l] && name[l] != '=')
        l++;
    if (!l)
        return NULL;
    for (char **e = __environ; *e; e++) {
        if (strncmp(name, *e, l) == 0 && (*e)[l] == '=')
            return *e + l + 1;
    }
    return NULL;
}

/* ── Arena allocator (blytc_arena.c globals) ────────────────────────────────── */

extern void *blytc_arena_base;
extern size_t blytc_arena_size;

/* ── Constructor: environment + malloc heap ─────────────────────────────────── */

__attribute__((constructor)) static void dap_native_init(void) {
    /* 1. Read /proc/self/environ, build __environ */
    int fd = n_openat(-100 /* AT_FDCWD */, "/proc/self/environ", 0 /* O_RDONLY */);
    if (fd >= 0) {
        int n = n_read(fd, s_env_buf, (int)(sizeof(s_env_buf) - 1));
        n_close(fd);
        if (n > 0) {
            s_env_buf[n] = '\0';
            int ei = 0, pos = 0;
            while (pos < n && ei < 1023) {
                s_env_ptrs[ei++] = s_env_buf + pos;
                while (pos < n && s_env_buf[pos] != '\0')
                    pos++;
                pos++;
            }
            s_env_ptrs[ei] = NULL;
            __environ = s_env_ptrs;
        }
    }

    /* 2. mmap a 4 MB arena for malloc */
    void *p = n_mmap(4u * 1024u * 1024u);
    /* On error mmap returns a large negative value (e.g. -ENOMEM cast to ptr) */
    if ((uintptr_t)p < (uintptr_t)0xFF000000u) {
        blytc_arena_base = p;
        blytc_arena_size = 4u * 1024u * 1024u;
    }
}

/* ── DAP server state ───────────────────────────────────────────────────────── */

#define MSG_MAX (128 * 1024)
#define MAX_BPS 256
#define MAX_SRC 512

typedef struct {
    char src[MAX_SRC];
    int line;
    int id;
} dap_bp_t;

static int g_listen_fd = -1;
static int g_client_fd = -1;
static int g_seq = 100;
static int g_configuration_done = 0;
static int g_step_mode = DAP_STEP_NONE;
static int g_step_base_depth = 0;
static int g_current_depth = 0;

static dap_bp_t g_bps[MAX_BPS];
static int g_n_bps = 0;
static int g_next_bp_id = 1;

static char g_send_buf[MSG_MAX];
static char g_recv_buf[MSG_MAX];

/* ── JSON helpers ───────────────────────────────────────────────────────────── */

static int jgi(const char *buf, const char *key, int def) {
    char k[64];
    snprintf(k, sizeof k, "\"%s\"", key);
    const char *p = strstr(buf, k);
    if (!p)
        return def;
    p = strchr(p, ':');
    if (!p)
        return def;
    while (*++p == ' ' || *p == '\t') {
    }
    return (*p == '"') ? def : (int)strtol(p, NULL, 10);
}

static int jgs(const char *buf, const char *key, char *out, size_t n) {
    char k[64];
    snprintf(k, sizeof k, "\"%s\"", key);
    const char *p = strstr(buf, k);
    if (!p)
        return 0;
    p = strchr(p, ':');
    if (!p)
        return 0;
    while (*++p == ' ' || *p == '\t') {
    }
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

static int jg_bp_lines(const char *buf, int *out, int max) {
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
        if (!lp)
            break;
        while (*++lp == ' ' || *lp == '\t') {
        }
        out[n++] = (int)strtol(lp, NULL, 10);
        p = lp;
    }
    return n;
}

/* ── Wire I/O ───────────────────────────────────────────────────────────────── */

static void send_raw(const char *data, size_t len) {
    const char *p = data;
    while (len) {
        int w = n_send(g_client_fd, p, len);
        if (w <= 0)
            return;
        p += w;
        len -= (size_t)w;
    }
}

static void send_msg(const char *json) {
    if (g_client_fd < 0)
        return;
    size_t jlen = strlen(json);
    char hdr[64];
    int hn = snprintf(hdr, sizeof hdr, "Content-Length: %zu\r\n\r\n", jlen);
    send_raw(hdr, (size_t)hn);
    send_raw(json, jlen);
}

static void send_resp(int req_seq, const char *cmd, int ok, const char *body) {
    int seq = ++g_seq;
    if (ok)
        snprintf(g_send_buf, sizeof g_send_buf,
                 "{\"seq\":%d,\"type\":\"response\",\"request_seq\":%d,"
                 "\"command\":\"%s\",\"success\":true,\"body\":%s}",
                 seq, req_seq, cmd, body ? body : "{}");
    else
        snprintf(g_send_buf, sizeof g_send_buf,
                 "{\"seq\":%d,\"type\":\"response\",\"request_seq\":%d,"
                 "\"command\":\"%s\",\"success\":false,\"message\":\"%s\"}",
                 seq, req_seq, cmd, body ? body : "");
    send_msg(g_send_buf);
}

static void send_ev(const char *ev, const char *body) {
    int seq = ++g_seq;
    snprintf(g_send_buf, sizeof g_send_buf,
             "{\"seq\":%d,\"type\":\"event\",\"event\":\"%s\",\"body\":%s}", seq, ev,
             body ? body : "{}");
    send_msg(g_send_buf);
}

/* Read one DAP message (Content-Length framing) into buf. Returns body len or -1. */
static int read_dap_msg(int fd, char *buf, int buf_size) {
    /* Read header byte-by-byte until \r\n\r\n */
    int hi = 0;
    while (hi + 4 <= buf_size) {
        if (n_recv_byte(fd, buf + hi) <= 0)
            return -1;
        hi++;
        if (hi >= 4 && buf[hi - 4] == '\r' && buf[hi - 3] == '\n' && buf[hi - 2] == '\r' &&
            buf[hi - 1] == '\n')
            break;
    }
    buf[hi] = 0;
    const char *cl = strstr(buf, "Content-Length:");
    if (!cl)
        return -1;
    int len = (int)strtol(cl + 15, NULL, 10);
    if (len <= 0 || len + 1 > buf_size)
        return -1;
    if (n_recv_all(fd, buf, len) < 0)
        return -1;
    buf[len] = 0;
    return len;
}

/* ── Request handlers ───────────────────────────────────────────────────────── */

static void do_initialize(int seq) {
    send_resp(seq, "initialize", 1,
              "{\"supportsConfigurationDoneRequest\":true,"
              "\"supportsLoadedSourcesRequest\":false,"
              "\"supportsRestartRequest\":false,"
              "\"supportsStepBack\":false,"
              "\"supportsTerminateRequest\":true}");
    send_ev("initialized", "{}");
}

static void do_set_breakpoints(int seq, const char *msg) {
    char source[MAX_SRC] = {0};
    const char *sp = strstr(msg, "\"source\"");
    if (sp)
        jgs(sp, "path", source, sizeof source);

    int lines[MAX_BPS], n = jg_bp_lines(msg, lines, MAX_BPS);

    /* Remove existing BPs for this source */
    int kept = 0;
    for (int i = 0; i < g_n_bps; i++) {
        if (strcmp(g_bps[i].src, source) != 0) {
            if (kept != i)
                g_bps[kept] = g_bps[i];
            kept++;
        }
    }
    g_n_bps = kept;

    /* Add new BPs */
    int first_new_id = g_next_bp_id + 1;
    for (int i = 0; i < n && g_n_bps < MAX_BPS; i++) {
        dap_bp_t *bp = &g_bps[g_n_bps++];
        snprintf(bp->src, sizeof bp->src, "%s", source);
        bp->line = lines[i];
        bp->id = ++g_next_bp_id;
    }

    static char body[MSG_MAX / 4];
    int off = snprintf(body, sizeof body, "{\"breakpoints\":[");
    for (int i = 0; i < n; i++) {
        off += snprintf(body + off, sizeof(body) - (size_t)off,
                        "%s{\"id\":%d,\"verified\":%s,\"line\":%d}", i ? "," : "", first_new_id + i,
                        lines[i] > 0 ? "true" : "false", lines[i]);
    }
    snprintf(body + off, sizeof(body) - (size_t)off, "]}");
    send_resp(seq, "setBreakpoints", 1, body);
}

/* Dispatch a protocol message.
 * Returns 1 if the caller should resume (continue/step/disconnect), 0 to loop. */
static int dispatch(const char *msg) {
    int seq = jgi(msg, "seq", 0);
    char cmd[64] = {0};
    jgs(msg, "command", cmd, sizeof cmd);

    if (strcmp(cmd, "initialize") == 0) {
        do_initialize(seq);
    } else if (strcmp(cmd, "launch") == 0) {
        send_resp(seq, "launch", 1, "{}");
    } else if (strcmp(cmd, "configurationDone") == 0) {
        g_configuration_done = 1;
        send_resp(seq, "configurationDone", 1, "{}");
    } else if (strcmp(cmd, "setBreakpoints") == 0) {
        do_set_breakpoints(seq, msg);
    } else if (strcmp(cmd, "threads") == 0) {
        send_resp(seq, "threads", 1, "{\"threads\":[{\"id\":1,\"name\":\"cart\"}]}");
    } else if (strcmp(cmd, "continue") == 0) {
        g_step_mode = DAP_STEP_NONE;
        send_resp(seq, "continue", 1, "{\"allThreadsContinued\":true}");
        send_ev("continued", "{\"threadId\":1,\"allThreadsContinued\":true}");
        return 1;
    } else if (strcmp(cmd, "next") == 0) {
        g_step_mode = DAP_STEP_OVER;
        g_step_base_depth = g_current_depth;
        send_resp(seq, "next", 1, "{\"allThreadsContinued\":true}");
        send_ev("continued", "{\"threadId\":1,\"allThreadsContinued\":true}");
        return 1;
    } else if (strcmp(cmd, "stepIn") == 0) {
        g_step_mode = DAP_STEP_IN;
        g_step_base_depth = g_current_depth;
        send_resp(seq, "stepIn", 1, "{\"allThreadsContinued\":true}");
        send_ev("continued", "{\"threadId\":1,\"allThreadsContinued\":true}");
        return 1;
    } else if (strcmp(cmd, "stepOut") == 0) {
        g_step_mode = DAP_STEP_OUT;
        g_step_base_depth = g_current_depth;
        send_resp(seq, "stepOut", 1, "{\"allThreadsContinued\":true}");
        send_ev("continued", "{\"threadId\":1,\"allThreadsContinued\":true}");
        return 1;
    } else if (strcmp(cmd, "pause") == 0) {
        g_step_mode = DAP_STEP_IN; /* break at next line */
        send_resp(seq, "pause", 1, "{}");
    } else if (strcmp(cmd, "disconnect") == 0 || strcmp(cmd, "terminate") == 0) {
        send_resp(seq, cmd, 1, "{}");
        return 1;
    } else {
        send_resp(seq, cmd, 1, "{}");
    }
    return 0;
}

/* ── Public API ─────────────────────────────────────────────────────────────── */

int fc_consolelua_dap_listen(int port) {
    int fd = n_socket(2 /* AF_INET */, 1 /* SOCK_STREAM */, 0);
    if (fd < 0)
        return -1;

    int one = 1;
    n_setsockopt(fd, 1 /* SOL_SOCKET  */, 2 /* SO_REUSEADDR */, &one, 4);
    n_setsockopt(fd, 6 /* IPPROTO_TCP */, 1 /* TCP_NODELAY  */, &one, 4);

    n_saddr_t addr = {0};
    addr.sin_family = 2; /* AF_INET */
    addr.sin_port = n_htons((unsigned short)port);
    addr.sin_addr = 0x0100007Fu; /* 127.0.0.1 in LE */

    if (n_bind(fd, &addr, sizeof addr) < 0) {
        n_close(fd);
        return -1;
    }

    int alen = (int)sizeof addr;
    if (n_getsockname(fd, &addr, &alen) < 0) {
        n_close(fd);
        return -1;
    }
    int actual = (int)n_htons(addr.sin_port);

    if (n_listen(fd, 1) < 0) {
        n_close(fd);
        return -1;
    }

    g_listen_fd = fd;
    g_client_fd = -1;
    g_configuration_done = 0;
    return actual;
}

void fc_consolelua_dap_shutdown(void) {
    if (g_client_fd >= 0) {
        n_close(g_client_fd);
        g_client_fd = -1;
    }
    if (g_listen_fd >= 0) {
        n_close(g_listen_fd);
        g_listen_fd = -1;
    }
}

int fc_dap_wait_configuration_done(void) {
    if (g_listen_fd < 0)
        return 0;

    n_saddr_t cli = {0};
    int alen = (int)sizeof cli;
    g_client_fd = n_accept4(g_listen_fd, &cli, &alen, 0);
    if (g_client_fd < 0)
        return 0;

    int one = 1;
    n_setsockopt(g_client_fd, 6, 1, &one, 4);

    g_configuration_done = 0;
    for (;;) {
        int n = read_dap_msg(g_client_fd, g_recv_buf, (int)sizeof g_recv_buf - 1);
        if (n < 0)
            return 0;
        int resume = dispatch(g_recv_buf);
        if (g_configuration_done)
            return 1;
        if (resume)
            return 0; /* unexpected disconnect during config */
    }
}

void fc_dap_host_send(const char *json, size_t len) {
    (void)len;
    send_msg(json);
}

int fc_dap_host_recv(char *buf, size_t max_len) {
    for (;;) {
        if (g_client_fd < 0)
            return 0;
        int n = read_dap_msg(g_client_fd, g_recv_buf, (int)sizeof g_recv_buf - 1);
        if (n < 0)
            return 0;

        char cmd[64] = {0};
        jgs(g_recv_buf, "command", cmd, sizeof cmd);

        /* Inspection commands: surface to caller (master_hook_native.c) */
        if (strcmp(cmd, "stackTrace") == 0 || strcmp(cmd, "scopes") == 0 ||
            strcmp(cmd, "variables") == 0 || strcmp(cmd, "evaluate") == 0 ||
            strcmp(cmd, "threads") == 0) {
            size_t cp = (size_t)n < max_len - 1 ? (size_t)n : max_len - 1;
            memcpy(buf, g_recv_buf, cp);
            buf[cp] = 0;
            return (int)cp;
        }

        /* Protocol messages: dispatch; resume signal breaks pause loop */
        if (dispatch(g_recv_buf))
            return 0;
    }
}

int fc_dap_check_hook_line(const char *source, int line, int depth) {
    if (g_client_fd < 0 || !g_configuration_done)
        return 0;

    int should_break = 0;
    const char *reason = "breakpoint";

    if (g_step_mode == DAP_STEP_IN) {
        should_break = 1;
        reason = "step";
    } else if (g_step_mode == DAP_STEP_OVER) {
        should_break = (depth <= g_step_base_depth);
        if (should_break)
            reason = "step";
    } else if (g_step_mode == DAP_STEP_OUT) {
        should_break = (depth < g_step_base_depth);
        if (should_break)
            reason = "step";
    } else {
        /* Check breakpoints — compare basenames for robustness */
        const char *sb = source;
        for (const char *q = source; *q; q++)
            if (*q == '/')
                sb = q + 1;
        for (int i = 0; i < g_n_bps && !should_break; i++) {
            if (g_bps[i].line != line)
                continue;
            const char *bb = g_bps[i].src;
            for (const char *q = g_bps[i].src; *q; q++)
                if (*q == '/')
                    bb = q + 1;
            if (strcmp(sb, bb) == 0)
                should_break = 1;
        }
    }

    if (!should_break)
        return 0;

    g_current_depth = depth;
    g_step_mode = DAP_STEP_NONE;

    char body[256];
    snprintf(body, sizeof body, "{\"reason\":\"%s\",\"threadId\":1,\"allThreadsStopped\":true}",
             reason);
    send_ev("stopped", body);
    return 1;
}

/* ── Unused API stubs ────────────────────────────────────────────────────────── */

int fc_dap_configuration_done(void) {
    return g_configuration_done;
}
void fc_dap_poll_messages(void) {
}
int fc_dap_hook_yielded(void) {
    return 0;
}
int fc_dap_continue_pending(void) {
    return 0;
}
void fc_dap_do_resume(void) {
}
void fc_dap_emit_loaded_source(const char *p) {
    (void)p;
}
void fc_dap_output(const char *msg) {
    extern void blyt_console_debug(const char *) __attribute__((weak));
    if (blyt_console_debug)
        blyt_console_debug(msg);
}
