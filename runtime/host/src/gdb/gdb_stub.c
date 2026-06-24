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

#include "blyt_trace.h"
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
    /* 1 while in the initial startup/connect halt (no client interaction
     * yet).  Breakpoint, step, and interrupt halts set this to 0 so
     * fc_gdb_stub_continue_initial_halt() never clears a real halt: both
     * kinds park the stub at halted=1/pending_action=-1, so pending_action
     * alone cannot distinguish them. */
    int initial_halt;
    /* Set to 1 when the transport is initialised; cleared after the first
     * vCont;c so we can simulate an entry-point stop and let VS Code enable
     * all debug controls before the user explicitly continues. */
    int entry_stop_pending;
} gdb_state_t;

#ifdef BLYT_GDB_TCP
static gdb_state_t g_gdb = {
    .mu = PTHREAD_MUTEX_INITIALIZER, .pending_action = -1, .initial_halt = 1};
#else
static gdb_state_t g_gdb = {.pending_action = -1, .initial_halt = 1};
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
    blyt_tracef(BLYT_TRACE_GDB, "send %s", payload);
    GDB_LOCK();
    g_gdb.transport->send_pkt(payload);
    GDB_UNLOCK();
}

/* Build a T05 stop reply with all 33 register values inline.
 * LLDB caches these and skips individual p-register round trips on stops,
 * which eliminates ~33 WebSocket exchanges per conditional breakpoint hit. */
/* `reason` is the stop-reason field (e.g. "swbreak:;" or "library:;"), inserted
 * right after "T05" and before "thread:01;". */
static void send_stop_with_regs_reason(const char *reason) {
    if (!g_gdb.ops.read_regs) {
        char small[64];
        snprintf(small, sizeof small, "T05%sthread:01;", reason);
        send_response(small);
        return;
    }
    uint8_t regs[33 * 4];
    g_gdb.ops.read_regs(regs);
    /* 22 chars header + 33 × 12 ("xx:xxxxxxxx;") + NUL = ~420 bytes */
    char buf[512];
    int n = snprintf(buf, sizeof buf, "T05%sthread:01;", reason);
    for (int i = 0; i < 33; i++) {
        const uint8_t *r = regs + i * 4;
        n += snprintf(buf + n, (int)sizeof(buf) - n, "%02x:%02x%02x%02x%02x;", i, r[0], r[1], r[2],
                      r[3]);
    }
    send_response(buf);
}

static void send_stop_with_regs(void) {
    send_stop_with_regs_reason("swbreak:;");
}

static void clear_all_breakpoints(void) {
    if (g_gdb.ops.clear_breakpoint) {
        for (int i = 0; i < g_gdb.n_breaks; i++)
            g_gdb.ops.clear_breakpoint(g_gdb.breaks[i]);
    }
    g_gdb.n_breaks = 0;
}

/* ── packet handlers ──────────────────────────────────────────────────────── */

static void handle_qSupported(char *out, size_t cap) {
    snprintf(out, cap,
             "PacketSize=4000;qXfer:libraries-svr4:read+;"
             "qXfer:exec-file:read+;qXfer:features:read+;"
             "swbreak+;vContSupported+;X-");
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

static void handle_qXfer_features(const char *args, char *out, size_t cap) {
    if (strncmp(args, "target.xml:", 11) != 0) {
        snprintf(out, cap, "E01");
        return;
    }
    static const char xml[] = "<?xml version=\"1.0\"?>"
                              "<!DOCTYPE target SYSTEM \"gdb-target.dtd\">"
                              "<target version=\"1.0\">"
                              "<architecture>riscv:rv32</architecture>"
                              "</target>";
    snprintf(out, cap, "l%s", xml);
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

/* Returns 0 when a response has been written to out, 1 when the response is
 * deferred (vCont;c / vCont;s — T05 will come from fc_gdb_stub_notify_stopped
 * when the cart actually stops). */
static int handle_vCont(const char *args, char *out, size_t cap) {
    int action = (strchr(args, 's') || strchr(args, 'S')) ? 1 : 0;
    GDB_LOCK();
    if (action == 0 && g_gdb.entry_stop_pending) {
        /* First vCont;c after connection: simulate an entry-point stop so
         * VS Code enables the full set of debug controls (step/continue/etc.)
         * before the user explicitly continues.  The cart stays halted until
         * the next vCont;c. */
        g_gdb.entry_stop_pending = 0;
        g_gdb.halted = 1;
        g_gdb.pending_action = -1;
        g_gdb.initial_halt = 0;
        GDB_UNLOCK();
        send_stop_with_regs();
        return 1; /* deferred — response already sent via send_stop_with_regs */
    }
    g_gdb.pending_action = action;
    g_gdb.halted = 0;
    g_gdb.initial_halt = 0;
    GDB_UNLOCK();
    /* No immediate response — T05 comes from fc_gdb_stub_notify_stopped. */
    return 1;
}

static void handle_packet(const char *pkt) {
    static char out[MAX_PACKET];
    out[0] = '\0';

    /* Inbound trace at the stub level covers the TCP and WASM transports. */
    if (pkt[0] == '\x03' && pkt[1] == '\0')
        blyt_tracef(BLYT_TRACE_GDB, "recv ^C (interrupt)");
    else
        blyt_tracef(BLYT_TRACE_GDB, "recv %s", pkt);

    if (strncmp(pkt, "qSupported", 10) == 0) {
        handle_qSupported(out, sizeof out);
    } else if (strncmp(pkt, "qXfer:exec-file:read:", 21) == 0) {
        handle_qXfer_exec_file(pkt + 21, out, sizeof out);
    } else if (strncmp(pkt, "qXfer:libraries-svr4:read:", 26) == 0) {
        handle_qXfer_libraries(pkt + 26, out, sizeof out);
    } else if (strncmp(pkt, "qXfer:features:read:", 20) == 0) {
        handle_qXfer_features(pkt + 20, out, sizeof out);
    } else if (pkt[0] == '?' && pkt[1] == '\0') {
        /* Include register values so lldb skips individual p-register queries.
         * On the initial attach stop, carry the `library:` reason when a cart
         * library is present (issue #119): lldb-dap's `program` is a stub ELF
         * with no loader rendezvous, so nothing would otherwise prompt lldb to
         * read qXfer:libraries-svr4 — and the cart (a shared library) would stay
         * unknown, leaving breakpoints pending until the first reload.  The
         * `library:` reason makes lldb fetch the svr4 list at attach so cart
         * breakpoints bind (with an address) before init() runs. */
        if (g_gdb.initial_halt && g_gdb.layout.n_libraries > 0)
            send_stop_with_regs_reason("library:;");
        else
            send_stop_with_regs();
        return;
    } else if (pkt[0] == 'c' && (pkt[1] == '\0' || pkt[1] == ';')) {
        /* Legacy continue — treat identically to vCont;c. */
        if (handle_vCont(";c", out, sizeof out))
            return; /* deferred */
    } else if (pkt[0] == 's' && (pkt[1] == '\0' || pkt[1] == ';')) {
        /* Legacy single-step — treat identically to vCont;s. */
        if (handle_vCont(";s", out, sizeof out))
            return; /* deferred */
    } else if (pkt[0] == 'p' && pkt[1] != '\0') {
        /* Read single register (lldb prefers this over 'g' when
         * QThreadSuffixSupported is enabled).  Ignore optional ;thread:NN. */
        uint32_t regnum = (uint32_t)strtoul(pkt + 1, NULL, 16);
        if (regnum < 33 && g_gdb.ops.read_regs) {
            uint8_t regs[33 * 4];
            g_gdb.ops.read_regs(regs);
            const uint8_t *r = regs + regnum * 4;
            snprintf(out, sizeof out, "%02x%02x%02x%02x", r[0], r[1], r[2], r[3]);
        } else {
            snprintf(out, sizeof out, "E00");
        }
    } else if (pkt[0] == 'P' && pkt[1] != '\0') {
        /* Write single register (lldb uses this in preference to G). */
        const char *colon = strchr(pkt + 1, ':');
        if (!colon || !g_gdb.ops.write_regs) {
            snprintf(out, sizeof out, "E00");
        } else {
            uint32_t regnum = (uint32_t)strtoul(pkt + 1, NULL, 16);
            if (regnum >= 33) {
                snprintf(out, sizeof out, "E00");
            } else {
                uint8_t regs[33 * 4];
                g_gdb.ops.read_regs(regs);
                hex_decode(colon + 1, regs + regnum * 4, 4);
                g_gdb.ops.write_regs(regs);
                snprintf(out, sizeof out, "OK");
            }
        }
    } else if (strcmp(pkt, "qProcessInfo") == 0) {
        /* Target process info: identify as RISC-V 32-bit so lldb can set up
         * the correct register layout and disassembler. */
        snprintf(out, sizeof out,
                 "pid:00000001;triple:riscv32--none-elf;endian:little;ptrsize:04;");
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
        if (handle_vCont(pkt + 5, out, sizeof out))
            return; /* deferred — T05 comes from fc_gdb_stub_notify_stopped */
    } else if (strcmp(pkt, "QStartNoAckMode") == 0) {
        /* lldb-dap requests no-ack mode for efficiency.  Acknowledge it; the
         * WASM stub continues to send '+' acks which lldb tolerates in this
         * direction even after switching modes. */
        snprintf(out, sizeof out, "OK");
    } else if (strcmp(pkt, "QThreadSuffixSupported") == 0) {
        /* lldb extension: client will append ;thread:NN to register packets.
         * Respond OK to acknowledge; we parse and ignore the suffix. */
        snprintf(out, sizeof out, "OK");
    } else if (pkt[0] == 'H') {
        /* Set thread for subsequent operations.  Single-threaded target: any
         * thread selector is fine; always acknowledge with OK. */
        snprintf(out, sizeof out, "OK");
    } else if (pkt[0] == 'T' && pkt[1] != '\0') {
        /* Thread alive query.  We have only thread 1. */
        long tid = strtol(pkt + 1, NULL, 16);
        snprintf(out, sizeof out, tid == 1 ? "OK" : "E01");
    } else if (strcmp(pkt, "qfThreadInfo") == 0) {
        /* First thread-info query: report thread 1. */
        snprintf(out, sizeof out, "m1");
    } else if (strcmp(pkt, "qsThreadInfo") == 0) {
        /* Subsequent thread-info query: end of list. */
        snprintf(out, sizeof out, "l");
    } else if (strncmp(pkt, "qThreadStopInfo", 15) == 0) {
        /* Per-thread stop reason query (lldb extension).  We only have thread 1;
         * if halted, include registers so lldb skips follow-up p-register queries. */
        if (g_gdb.halted) {
            send_stop_with_regs();
            return;
        } else {
            snprintf(out, sizeof out, "T00:;");
        }
    } else if (strncmp(pkt, "qRegisterInfo", 13) == 0) {
        /* Per-register metadata for RISC-V rv32 (x0-x31 = regs 0x00-0x1f,
         * pc = reg 0x20).  lldb queries these in sequence until it receives
         * E45 (end of list).  Providing full metadata lets lldb resolve DWARF
         * frame info and set software breakpoints at source lines. */
        static const char *const rv32_names[33] = {
            "x0",  "x1",  "x2",  "x3",  "x4",  "x5",  "x6",  "x7",  "x8",  "x9",  "x10",
            "x11", "x12", "x13", "x14", "x15", "x16", "x17", "x18", "x19", "x20", "x21",
            "x22", "x23", "x24", "x25", "x26", "x27", "x28", "x29", "x30", "x31", "pc"};
        static const char *const rv32_alts[33] = {
            "zero", "ra", "sp", "gp", "tp",  "t0",  "t1", "t2", "s0", "s1", "a0",
            "a1",   "a2", "a3", "a4", "a5",  "a6",  "a7", "s2", "s3", "s4", "s5",
            "s6",   "s7", "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6", NULL};
        static const char *const rv32_generic[33] = {
            NULL,   "ra",   "sp",   NULL,   NULL,   NULL,   NULL,   NULL, "fp", NULL, "arg1",
            "arg2", "arg3", "arg4", "arg5", "arg6", "arg7", "arg8", NULL, NULL, NULL, NULL,
            NULL,   NULL,   NULL,   NULL,   NULL,   NULL,   NULL,   NULL, NULL, NULL, "pc"};
        /* RISC-V DWARF: x0-x31 = 0-31, pc = 65. */
        static const int rv32_dwarf[33] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10,
                                           11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21,
                                           22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 65};
        uint32_t n = (uint32_t)strtoul(pkt + 13, NULL, 16);
        if (n >= 33) {
            snprintf(out, sizeof out, "E45");
        } else {
            int nn = snprintf(out, sizeof out,
                              "name:%s;bitsize:32;offset:%u;"
                              "encoding:uint;format:hex;"
                              "set:General Purpose Registers;"
                              "gcc:%d;dwarf:%d;",
                              rv32_names[n], n * 4, rv32_dwarf[n], rv32_dwarf[n]);
            if (rv32_alts[n])
                nn += snprintf(out + nn, sizeof(out) - nn, "alt-name:%s;", rv32_alts[n]);
            if (rv32_generic[n])
                snprintf(out + nn, sizeof(out) - nn, "generic:%s;", rv32_generic[n]);
        }
    } else if (pkt[0] == '\x03' && pkt[1] == '\0') {
        /* Out-of-band interrupt (\x03): halt the target immediately and send
         * T02 (SIGINT) so the debugger shows the thread as paused. */
        GDB_LOCK();
        g_gdb.halted = 1;
        g_gdb.pending_action = -1;
        g_gdb.initial_halt = 0;
        GDB_UNLOCK();
        send_response("T02thread:01;");
        return; /* response already sent; skip final send_response() */
    } else if (strcmp(pkt, "qAttached") == 0) {
        snprintf(out, sizeof out, "1");
    } else if (strncmp(pkt, "qC", 2) == 0) {
        snprintf(out, sizeof out, "QC1");
    } else if (pkt[0] == 'D' && (pkt[1] == '\0' || pkt[1] == ';')) {
        /* Detach: resume the cart and acknowledge so lldb-dap exits cleanly. */
        GDB_LOCK();
        g_gdb.pending_action = 0; /* continue */
        g_gdb.halted = 0;
        GDB_UNLOCK();
        fc_gdb_stub_set_has_client(0);
        snprintf(out, sizeof out, "OK");
    } else if (pkt[0] == 'k' && pkt[1] == '\0') {
        /* Kill: acknowledge and signal the cart to halt. */
        GDB_LOCK();
        g_gdb.pending_action = 2; /* exit */
        GDB_UNLOCK();
        snprintf(out, sizeof out, "W00");
    }
    /* Always send a response.  In no-ack mode (QStartNoAckMode) the client
     * waits indefinitely if no response arrives.  Empty string produces the
     * GDB RSP "unsupported" response ($#00). */
    send_response(out);
}

/* ── Public API ───────────────────────────────────────────────────────────── */

void fc_gdb_stub_set_transport(const fc_gdb_transport_t *t) {
    g_gdb.transport = t;
    g_gdb.has_client = 0;
    g_gdb.entry_stop_pending = 0; /* disabled — stopOnEntry:true handles entry stop */
    /* Start in halted/pending=-1 state so the WASM run loop stays in
     * BLYT_DEBUG_PAUSED_GDB until the user explicitly sends vCont;c.
     * Without this, pending_action=0 (C zero-init) causes the PAUSED
     * re-entry to fall through immediately, bypassing check_break and
     * letting on_ebreak advance PC past the first breakpoint to addr+4. */
    g_gdb.halted = 1;
    g_gdb.pending_action = -1;
    g_gdb.initial_halt = 1;
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
    g_gdb.initial_halt = 0;
    GDB_UNLOCK();
    send_stop_with_regs();
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

void fc_gdb_stub_set_has_client(int val) {
    if (val == 0) {
        /* Client disconnected: clear all breakpoints so the cart does not hit
         * dangling ebreaks, then resume so the run loop is not permanently
         * blocked waiting for a vCont that will never arrive. */
        clear_all_breakpoints();
        GDB_LOCK();
        g_gdb.has_client = 0;
        g_gdb.halted = 0;
        g_gdb.pending_action = 0;
        GDB_UNLOCK();
    } else {
        /* New client connected: reset to initial-stop state so the client
         * can do its handshake and send vCont;c before execution resumes. */
        GDB_LOCK();
        g_gdb.has_client = 1;
        g_gdb.halted = 1;
        g_gdb.pending_action = -1;
        g_gdb.initial_halt = 1;
        GDB_UNLOCK();
    }
}

int fc_gdb_stub_is_halted(void) {
    GDB_LOCK();
    int h = g_gdb.halted;
    GDB_UNLOCK();
    return h;
}

/* In hybrid (DAP+GDB) mode the Lua DAP session has already received its
 * breakpoints before the frame loop starts.  Clear the initial halt so the
 * cart can run without waiting for a vCont;c from lldb-dap.  Only clears
 * only while still in the initial startup/connect halt (initial_halt set).
 * Breakpoint, step, and interrupt halts clear initial_halt when they fire,
 * so they are never released here — pending_action cannot distinguish them
 * (every halt parks at pending_action == -1). */
void fc_gdb_stub_continue_initial_halt(void) {
    GDB_LOCK();
    if (g_gdb.halted && g_gdb.pending_action < 0 && g_gdb.initial_halt) {
        g_gdb.pending_action = 0;
        g_gdb.halted = 0;
        g_gdb.initial_halt = 0;
    }
    GDB_UNLOCK();
}

#ifdef BLYT_GDB_TEST
/* Reset all stub state; used by unit tests between test cases. */
void fc_gdb_stub_test_reset(void) {
    memset(&g_gdb, 0, sizeof g_gdb);
    g_gdb.pending_action = -1;
    g_gdb.initial_halt = 1;
}
#endif

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
