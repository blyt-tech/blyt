/* GDB remote serial protocol stub — transport-agnostic.
 *
 * Adapted from spikes/spike-j/host/gdb_stub.c.  The TCP server thread and
 * socket code are removed; all I/O is routed through fc_gdb_transport_t.
 *
 * Protocol surface:
 *   qSupported, qXfer:exec-file:read, qXfer:libraries-svr4:read,
 *   ?, g, G, m, M, vCont, vCont?, Z0, z0, qAttached, qC
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef BLYT_GDB_TCP
#include <pthread.h>
#endif

#include "gdb_stub.h"

#define MAX_PACKET (1 << 16)
#define MAX_BREAKS 128

typedef struct {
#ifdef BLYT_GDB_TCP
    pthread_mutex_t mu;
#endif
    const fc_gdb_transport_t *transport;
    fc_gdb_layout_t layout;
    fc_gdb_cpu_ops_t ops;
    int has_client;

    uint32_t breaks[MAX_BREAKS];
    int n_breaks;

    /* 0 = running, 1 = halted waiting for vCont. */
    int halted;
    /* -1 = no action yet, 0 = continue, 1 = single-step, 2 = exit. */
    int pending_action;
} gdb_state_t;

#ifdef BLYT_GDB_TCP
static gdb_state_t g_gdb = {.mu = PTHREAD_MUTEX_INITIALIZER, .pending_action = -1};
#else
static gdb_state_t g_gdb = {.pending_action = -1};
#endif

/* ── mutex helpers (no-op on WASM — single-threaded) ─────────────────────── */

#ifdef BLYT_GDB_TCP
#define GDB_LOCK() pthread_mutex_lock(&g_gdb.mu)
#define GDB_UNLOCK() pthread_mutex_unlock(&g_gdb.mu)
#else
#define GDB_LOCK() ((void)0)
#define GDB_UNLOCK() ((void)0)
#endif

/* ── hex utilities ────────────────────────────────────────────────────────── */

static char hexnyb(int n) {
    return "0123456789abcdef"[n & 0xf];
}

static int from_hex(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static void hex_encode(const uint8_t *src, size_t n, char *dst) {
    for (size_t i = 0; i < n; i++) {
        dst[2 * i] = hexnyb(src[i] >> 4);
        dst[2 * i + 1] = hexnyb(src[i] & 0xf);
    }
    dst[2 * n] = '\0';
}

static int hex_decode(const char *src, uint8_t *dst, size_t cap) {
    size_t i = 0;
    while (src[2 * i] && src[2 * i + 1] && i < cap) {
        int hi = from_hex(src[2 * i]);
        int lo = from_hex(src[2 * i + 1]);
        if (hi < 0 || lo < 0)
            break;
        dst[i] = (uint8_t)((hi << 4) | lo);
        i++;
    }
    return (int)i;
}

/* ── internal send ────────────────────────────────────────────────────────── */

static void send_response(const char *payload) {
    if (!g_gdb.transport || !g_gdb.transport->send_pkt)
        return;
    GDB_LOCK();
    g_gdb.transport->send_pkt(payload);
    GDB_UNLOCK();
}

/* ── packet handlers ──────────────────────────────────────────────────────── */

static void handle_qSupported(char *out, size_t cap) {
    snprintf(out, cap,
             "PacketSize=4000;qXfer:libraries-svr4:read+;"
             "qXfer:exec-file:read+;swbreak+;vContSupported+");
}

static void handle_qXfer_exec_file(const char *args, char *out, size_t cap) {
    const char *p = strchr(args, ':');
    if (!p) {
        snprintf(out, cap, "E01");
        return;
    }
    p++;
    uint32_t off = (uint32_t)strtoul(p, (char **)&p, 16);
    if (*p == ',')
        p++;
    uint32_t len = (uint32_t)strtoul(p, NULL, 16);
    const char *path = g_gdb.layout.exec_path ? g_gdb.layout.exec_path : "";
    size_t plen = strlen(path);
    if (off >= (uint32_t)plen) {
        snprintf(out, cap, "l");
        return;
    }
    uint32_t avail = (uint32_t)(plen - off);
    if (len > avail)
        len = avail;
    char prefix = (off + len >= (uint32_t)plen) ? 'l' : 'm';
    if (cap < (size_t)len + 2) {
        snprintf(out, cap, "E02");
        return;
    }
    out[0] = prefix;
    memcpy(out + 1, path + off, len);
    out[1 + len] = '\0';
}

static void handle_qXfer_libraries(const char *args, char *out, size_t cap) {
    const char *p = strchr(args, ':');
    if (!p) {
        snprintf(out, cap, "E01");
        return;
    }
    p++;
    uint32_t off = (uint32_t)strtoul(p, (char **)&p, 16);
    if (*p == ',')
        p++;
    uint32_t len = (uint32_t)strtoul(p, NULL, 16);

    char xml[8192];
    int xn = snprintf(xml, sizeof xml, "<library-list-svr4 version=\"1.0\" main-lm=\"0x0\">");
    for (int i = 0; i < g_gdb.layout.n_libraries; i++) {
        const fc_gdb_library_t *lib = &g_gdb.layout.libraries[i];
        xn += snprintf(xml + xn, (int)sizeof(xml) - xn,
                       "<library name=\"%s\" lm=\"0x%x\" "
                       "l_addr=\"0x%x\" l_ld=\"0x%x\" lmid=\"0x0\"/>",
                       lib->path, lib->l_addr, lib->l_addr, lib->l_ld);
    }
    xn += snprintf(xml + xn, (int)sizeof(xml) - xn, "</library-list-svr4>");

    if (off >= (uint32_t)xn) {
        snprintf(out, cap, "l");
        return;
    }
    uint32_t avail = (uint32_t)xn - off;
    if (len > avail)
        len = avail;
    char prefix = (off + len >= (uint32_t)xn) ? 'l' : 'm';
    if (cap < (size_t)len + 2) {
        snprintf(out, cap, "E02");
        return;
    }
    out[0] = prefix;
    memcpy(out + 1, xml + off, len);
    out[1 + len] = '\0';
}

static void handle_g(char *out, size_t cap) {
    if (!g_gdb.ops.read_regs) {
        snprintf(out, cap, "E01");
        return;
    }
    uint8_t regs[33 * 4];
    g_gdb.ops.read_regs(regs);
    if (cap < sizeof(regs) * 2 + 1) {
        snprintf(out, cap, "E02");
        return;
    }
    hex_encode(regs, sizeof regs, out);
}

static void handle_G(const char *args, char *out, size_t cap) {
    if (!g_gdb.ops.write_regs) {
        snprintf(out, cap, "E01");
        return;
    }
    uint8_t regs[33 * 4];
    if (hex_decode(args, regs, sizeof regs) != (int)sizeof(regs)) {
        snprintf(out, cap, "E02");
        return;
    }
    g_gdb.ops.write_regs(regs);
    snprintf(out, cap, "OK");
}

static void handle_m(const char *args, char *out, size_t cap) {
    char *end;
    uint32_t addr = (uint32_t)strtoul(args, &end, 16);
    if (*end != ',') {
        snprintf(out, cap, "E01");
        return;
    }
    uint32_t len = (uint32_t)strtoul(end + 1, NULL, 16);
    if (!g_gdb.ops.read_mem) {
        snprintf(out, cap, "E02");
        return;
    }
    if (len > 4096 || cap < len * 2 + 1) {
        snprintf(out, cap, "E03");
        return;
    }
    uint8_t *buf = malloc(len);
    if (!buf) {
        snprintf(out, cap, "E04");
        return;
    }
    uint32_t got = g_gdb.ops.read_mem(addr, buf, len);
    hex_encode(buf, got, out);
    free(buf);
}

static void handle_M(const char *args, char *out, size_t cap) {
    char *end;
    uint32_t addr = (uint32_t)strtoul(args, &end, 16);
    if (*end != ',') {
        snprintf(out, cap, "E01");
        return;
    }
    uint32_t len = (uint32_t)strtoul(end + 1, &end, 16);
    if (*end != ':') {
        snprintf(out, cap, "E02");
        return;
    }
    end++;
    if (!g_gdb.ops.write_mem) {
        snprintf(out, cap, "E03");
        return;
    }
    uint8_t *buf = malloc(len);
    if (!buf) {
        snprintf(out, cap, "E04");
        return;
    }
    if (hex_decode(end, buf, len) != (int)len) {
        free(buf);
        snprintf(out, cap, "E05");
        return;
    }
    g_gdb.ops.write_mem(addr, buf, len);
    free(buf);
    snprintf(out, cap, "OK");
}

static void handle_Z0(const char *args, char *out, size_t cap) {
    char *end;
    uint32_t addr = (uint32_t)strtoul(args, &end, 16);
    if (g_gdb.n_breaks >= MAX_BREAKS) {
        snprintf(out, cap, "E01");
        return;
    }
    if (g_gdb.ops.set_breakpoint)
        g_gdb.ops.set_breakpoint(addr);
    g_gdb.breaks[g_gdb.n_breaks++] = addr;
    snprintf(out, cap, "OK");
}

static void handle_z0(const char *args, char *out, size_t cap) {
    char *end;
    uint32_t addr = (uint32_t)strtoul(args, &end, 16);
    int kept = 0;
    for (int i = 0; i < g_gdb.n_breaks; i++) {
        if (g_gdb.breaks[i] != addr) {
            if (kept != i)
                g_gdb.breaks[kept] = g_gdb.breaks[i];
            kept++;
        }
    }
    if (g_gdb.n_breaks != kept && g_gdb.ops.clear_breakpoint)
        g_gdb.ops.clear_breakpoint(addr);
    g_gdb.n_breaks = kept;
    snprintf(out, cap, "OK");
}

static void handle_vCont(const char *args, char *out, size_t cap) {
    int action = strchr(args, 's') ? 1 : 0;
    GDB_LOCK();
    g_gdb.pending_action = action;
    g_gdb.halted = 0;
    GDB_UNLOCK();
    /* No immediate response — next T05 is emitted when CPU stops. */
    out[0] = '\0';
}

static void handle_packet(const char *pkt) {
    static char out[MAX_PACKET];
    out[0] = '\0';

    if (strncmp(pkt, "qSupported", 10) == 0) {
        handle_qSupported(out, sizeof out);
    } else if (strncmp(pkt, "qXfer:exec-file:read:", 21) == 0) {
        handle_qXfer_exec_file(pkt + 21, out, sizeof out);
    } else if (strncmp(pkt, "qXfer:libraries-svr4:read:", 26) == 0) {
        handle_qXfer_libraries(pkt + 26, out, sizeof out);
    } else if (pkt[0] == '?' && pkt[1] == '\0') {
        snprintf(out, sizeof out, "T05");
    } else if (pkt[0] == 'g' && pkt[1] == '\0') {
        handle_g(out, sizeof out);
    } else if (pkt[0] == 'G') {
        handle_G(pkt + 1, out, sizeof out);
    } else if (pkt[0] == 'm') {
        handle_m(pkt + 1, out, sizeof out);
    } else if (pkt[0] == 'M') {
        handle_M(pkt + 1, out, sizeof out);
    } else if (strncmp(pkt, "Z0,", 3) == 0) {
        handle_Z0(pkt + 3, out, sizeof out);
    } else if (strncmp(pkt, "z0,", 3) == 0) {
        handle_z0(pkt + 3, out, sizeof out);
    } else if (strcmp(pkt, "vCont?") == 0) {
        snprintf(out, sizeof out, "vCont;c;C;s;S");
    } else if (strncmp(pkt, "vCont", 5) == 0) {
        handle_vCont(pkt + 5, out, sizeof out);
    } else if (strcmp(pkt, "qAttached") == 0) {
        snprintf(out, sizeof out, "1");
    } else if (strncmp(pkt, "qC", 2) == 0) {
        snprintf(out, sizeof out, "QC1");
    }
    /* Unsupported packets: empty response per RSP spec. */

    if (out[0] != '\0')
        send_response(out);
}

/* ── Public API ───────────────────────────────────────────────────────────── */

void fc_gdb_stub_set_transport(const fc_gdb_transport_t *t) {
    g_gdb.transport = t;
    g_gdb.has_client = 0;
}

void fc_gdb_stub_set_layout(const fc_gdb_layout_t *layout) {
    g_gdb.layout = *layout;
}

void fc_gdb_stub_set_cpu_ops(const fc_gdb_cpu_ops_t *ops) {
    g_gdb.ops = *ops;
}

int fc_gdb_stub_check_break(uint32_t pc) {
    for (int i = 0; i < g_gdb.n_breaks; i++) {
        if (g_gdb.breaks[i] == pc)
            return 1;
    }
    return 0;
}

void fc_gdb_stub_notify_stopped(void) {
    GDB_LOCK();
    g_gdb.halted = 1;
    g_gdb.pending_action = -1;
    GDB_UNLOCK();
    send_response("T05swbreak:;");
}

void fc_gdb_stub_poll(void) {
    if (!g_gdb.transport || !g_gdb.transport->recv_pkt)
        return;
    static char pkt[MAX_PACKET];
    int n = g_gdb.transport->recv_pkt(pkt, sizeof pkt);
    if (n < 0)
        return;
    handle_packet(pkt);
}

/* Process an already-received packet payload (used by TCP transport thread). */
void fc_gdb_stub_process_pkt(const char *pkt) {
    handle_packet(pkt);
}

int fc_gdb_stub_pending_action(void) {
    GDB_LOCK();
    int a = g_gdb.pending_action;
    GDB_UNLOCK();
    return a;
}

void fc_gdb_stub_block_until_resume(void) {
    if (g_gdb.transport && g_gdb.transport->on_stop)
        g_gdb.transport->on_stop();
}

int fc_gdb_stub_has_client(void) {
    return g_gdb.has_client;
}

/* Called by the TCP transport when a client connects/disconnects. */
void fc_gdb_stub_set_has_client(int val) {
    g_gdb.has_client = val;
}

void fc_gdb_stub_restore_bp_temp(uint32_t addr) {
    if (g_gdb.ops.clear_breakpoint)
        g_gdb.ops.clear_breakpoint(addr);
    /* breaks[] is intentionally not modified — repatch_bp uses it */
}

void fc_gdb_stub_repatch_bp(uint32_t addr) {
    for (int i = 0; i < g_gdb.n_breaks; i++) {
        if (g_gdb.breaks[i] == addr) {
            if (g_gdb.ops.set_breakpoint)
                g_gdb.ops.set_breakpoint(addr);
            return;
        }
    }
}
