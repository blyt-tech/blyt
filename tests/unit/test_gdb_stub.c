/* tests/unit/test_gdb_stub.c — unit tests for the GDB RSP stub.
 *
 * Compiles gdb_stub.c directly (no TCP transport, no pthreads) and drives
 * fc_gdb_stub_process_pkt() with a mock transport + CPU ops.
 *
 * Build via CMake target test_gdb_stub; run with ctest -R test_gdb_stub.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gdb_stub.h"

/* ── mock transport ──────────────────────────────────────────────────────── */

static char g_sent[65536];

static int mock_send(const char *payload) {
    strncpy(g_sent, payload, sizeof(g_sent) - 1);
    g_sent[sizeof(g_sent) - 1] = '\0';
    return 0;
}

static int mock_recv(char *buf, size_t cap) {
    (void)buf;
    (void)cap;
    return -1; /* no queued packets */
}

static void mock_on_stop(void) {
}

static const fc_gdb_transport_t mock_transport = {
    .send_pkt = mock_send,
    .recv_pkt = mock_recv,
    .on_stop = mock_on_stop,
};

/* ── mock CPU ops ────────────────────────────────────────────────────────── */

static uint8_t g_regs[33 * 4];
static uint8_t g_mem[4096];
static const uint32_t MEM_BASE = 0x10000;

static void mock_read_regs(uint8_t out[33 * 4]) {
    memcpy(out, g_regs, 33 * 4);
}
static void mock_write_regs(const uint8_t in[33 * 4]) {
    memcpy(g_regs, in, 33 * 4);
}

static uint32_t mock_read_mem(uint32_t addr, uint8_t *dst, uint32_t n) {
    if (addr < MEM_BASE || addr + n > MEM_BASE + (uint32_t)sizeof(g_mem))
        return 0;
    memcpy(dst, g_mem + (addr - MEM_BASE), n);
    return n;
}

static uint32_t mock_write_mem(uint32_t addr, const uint8_t *src, uint32_t n) {
    if (addr < MEM_BASE || addr + n > MEM_BASE + (uint32_t)sizeof(g_mem))
        return 0;
    memcpy(g_mem + (addr - MEM_BASE), src, n);
    return n;
}

static int g_bp_clear_count = 0;
static int mock_set_bp(uint32_t addr) {
    (void)addr;
    return 0;
}
static int mock_clear_bp(uint32_t addr) {
    (void)addr;
    g_bp_clear_count++;
    return 0;
}

static const fc_gdb_cpu_ops_t mock_ops = {
    .read_regs = mock_read_regs,
    .write_regs = mock_write_regs,
    .read_mem = mock_read_mem,
    .write_mem = mock_write_mem,
    .set_breakpoint = mock_set_bp,
    .clear_breakpoint = mock_clear_bp,
};

/* ── test helpers ────────────────────────────────────────────────────────── */

/* Declared in gdb_stub.c under BLYT_GDB_TEST. */
void fc_gdb_stub_test_reset(void);

static void reset_stub(void) {
    fc_gdb_stub_test_reset();
    memset(g_regs, 0, sizeof g_regs);
    memset(g_mem, 0, sizeof g_mem);
    g_sent[0] = '\0';
    g_bp_clear_count = 0;
    fc_gdb_stub_set_transport(&mock_transport);
    fc_gdb_stub_set_cpu_ops(&mock_ops);
    static const fc_gdb_layout_t empty_layout = {0};
    fc_gdb_stub_set_layout(&empty_layout);
}

static const char *drive(const char *pkt) {
    g_sent[0] = '\0';
    fc_gdb_stub_process_pkt(pkt);
    return g_sent;
}

static int failures;

#define ASSERT(cond, msg)                                                                          \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL [%s]: %s\n", __func__, msg);                                     \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

#define ASSERT_STR_EQ(a, b)                                                                        \
    do {                                                                                           \
        if (strcmp((a), (b)) != 0) {                                                               \
            fprintf(stderr, "FAIL [%s]: expected \"%s\", got \"%s\"\n", __func__, (b), (a));       \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

#define ASSERT_CONTAINS(haystack, needle)                                                          \
    do {                                                                                           \
        if (!strstr((haystack), (needle))) {                                                       \
            fprintf(stderr, "FAIL [%s]: \"%s\" not found in \"%s\"\n", __func__, (needle),         \
                    (haystack));                                                                   \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

/* ── handshake / session tests ───────────────────────────────────────────── */

static void test_qSupported(void) {
    reset_stub();
    const char *r = drive("qSupported:multiprocess+;qXfer:exec-file:read+");
    ASSERT_CONTAINS(r, "PacketSize");
    ASSERT_CONTAINS(r, "swbreak+");
    ASSERT_CONTAINS(r, "vContSupported+");
    ASSERT_CONTAINS(r, "qXfer:libraries-svr4:read+");
    ASSERT_CONTAINS(r, "qXfer:exec-file:read+");
    ASSERT_CONTAINS(r, "qXfer:features:read+");
    ASSERT_CONTAINS(r, "X-");
}

static void test_qAttached(void) {
    reset_stub();
    ASSERT_STR_EQ(drive("qAttached"), "1");
}

static void test_qC(void) {
    reset_stub();
    ASSERT_STR_EQ(drive("qC"), "QC1");
}

static void test_vContQ(void) {
    reset_stub();
    const char *r = drive("vCont?");
    ASSERT_CONTAINS(r, "vCont;");
    ASSERT_CONTAINS(r, ";c");
    ASSERT_CONTAINS(r, ";s");
}

static void test_qfThreadInfo(void) {
    reset_stub();
    ASSERT_STR_EQ(drive("qfThreadInfo"), "m1");
    ASSERT_STR_EQ(drive("qsThreadInfo"), "l");
}

static void test_H(void) {
    reset_stub();
    ASSERT_STR_EQ(drive("Hg0"), "OK");
    ASSERT_STR_EQ(drive("Hc-1"), "OK");
}

static void test_QStartNoAckMode(void) {
    reset_stub();
    ASSERT_STR_EQ(drive("QStartNoAckMode"), "OK");
}

static void test_QThreadSuffixSupported(void) {
    reset_stub();
    ASSERT_STR_EQ(drive("QThreadSuffixSupported"), "OK");
}

static void test_question_mark(void) {
    reset_stub();
    const char *r = drive("?");
    ASSERT_CONTAINS(r, "T05");
}

static void test_unknown_packet(void) {
    reset_stub();
    /* Unknown packet → empty response (GDB RSP "unsupported"). */
    ASSERT_STR_EQ(drive("qUnknownPacketXYZ"), "");
}

/* ── register tests ──────────────────────────────────────────────────────── */

static void test_g_register_read(void) {
    reset_stub();
    /* Set PC (register 32) to 0x00004000 little-endian. */
    g_regs[32 * 4 + 0] = 0x00;
    g_regs[32 * 4 + 1] = 0x40;
    g_regs[32 * 4 + 2] = 0x00;
    g_regs[32 * 4 + 3] = 0x00;
    const char *r = drive("g");
    ASSERT((int)strlen(r) == 33 * 4 * 2, "g reply must be 264 hex chars");
    /* Last 8 chars are PC little-endian = 00400000 */
    ASSERT_STR_EQ(r + 256, "00400000");
}

static void test_p_single_register(void) {
    reset_stub();
    /* Set x1 (ra) = 0x1234ABCD little-endian. */
    g_regs[1 * 4 + 0] = 0xCD;
    g_regs[1 * 4 + 1] = 0xAB;
    g_regs[1 * 4 + 2] = 0x34;
    g_regs[1 * 4 + 3] = 0x12;
    ASSERT_STR_EQ(drive("p1"), "cdab3412");
}

static void test_p_pc_register(void) {
    reset_stub();
    /* PC = register 32 = 0x20 hex. */
    const char *r = drive("p20");
    ASSERT((int)strlen(r) == 8, "p20 reply must be 8 hex chars");
}

static void test_p_out_of_range(void) {
    reset_stub();
    /* Register 33 = 0x21 hex → beyond end. */
    ASSERT_STR_EQ(drive("p21"), "E00");
}

static void test_G_write_registers(void) {
    reset_stub();
    /* Build 264-char hex string with PC=0x00008000. */
    char hex[265];
    memset(hex, '0', 264);
    hex[264] = '\0';
    /* PC is bytes 128..131 of the register buffer (register 32 × 4 bytes).
     * little-endian 0x00008000 → 00 80 00 00 → hex "00800000" at offset 256. */
    hex[256] = '0';
    hex[257] = '0';
    hex[258] = '8';
    hex[259] = '0';
    hex[260] = '0';
    hex[261] = '0';
    hex[262] = '0';
    hex[263] = '0';
    char pkt[270];
    snprintf(pkt, sizeof pkt, "G%s", hex);
    ASSERT_STR_EQ(drive(pkt), "OK");
    /* Verify g_regs PC was updated. */
    ASSERT(g_regs[32 * 4 + 0] == 0x00, "PC byte 0");
    ASSERT(g_regs[32 * 4 + 1] == 0x80, "PC byte 1");
    ASSERT(g_regs[32 * 4 + 2] == 0x00, "PC byte 2");
    ASSERT(g_regs[32 * 4 + 3] == 0x00, "PC byte 3");
}

/* ── memory tests ────────────────────────────────────────────────────────── */

static void test_m_read(void) {
    reset_stub();
    g_mem[0] = 0xDE;
    g_mem[1] = 0xAD;
    g_mem[2] = 0xBE;
    g_mem[3] = 0xEF;
    /* m10000,4 → 4 bytes at MEM_BASE */
    ASSERT_STR_EQ(drive("m10000,4"), "deadbeef");
}

static void test_m_read_returns_empty_for_unmapped(void) {
    reset_stub();
    /* Address 0 is below MEM_BASE; mock_read_mem returns 0 bytes → empty hex. */
    ASSERT_STR_EQ(drive("m0,4"), "");
}

static void test_M_write(void) {
    reset_stub();
    ASSERT_STR_EQ(drive("M10000,4:deadbeef"), "OK");
    ASSERT(g_mem[0] == 0xDE, "byte 0 = 0xDE");
    ASSERT(g_mem[1] == 0xAD, "byte 1 = 0xAD");
    ASSERT(g_mem[2] == 0xBE, "byte 2 = 0xBE");
    ASSERT(g_mem[3] == 0xEF, "byte 3 = 0xEF");
}

/* ── breakpoint tests ────────────────────────────────────────────────────── */

static void test_Z0_set_and_check(void) {
    reset_stub();
    ASSERT_STR_EQ(drive("Z0,10000,4"), "OK");
    ASSERT(fc_gdb_stub_check_break(0x10000) == 1, "check_break hit");
    ASSERT(fc_gdb_stub_check_break(0x10004) == 0, "check_break miss");
}

static void test_check_break_empty_table(void) {
    reset_stub();
    ASSERT(fc_gdb_stub_check_break(0x10000) == 0, "empty table → no hit");
}

static void test_Z0_multiple(void) {
    reset_stub();
    drive("Z0,1000,4");
    drive("Z0,2000,4");
    drive("Z0,3000,4");
    ASSERT(fc_gdb_stub_check_break(0x1000) == 1, "bp1");
    ASSERT(fc_gdb_stub_check_break(0x2000) == 1, "bp2");
    ASSERT(fc_gdb_stub_check_break(0x3000) == 1, "bp3");
    ASSERT(fc_gdb_stub_check_break(0x4000) == 0, "no bp4");
}

static void test_z0_remove(void) {
    reset_stub();
    drive("Z0,1000,4");
    drive("Z0,2000,4");
    ASSERT_STR_EQ(drive("z0,1000,4"), "OK");
    ASSERT(fc_gdb_stub_check_break(0x1000) == 0, "bp1 removed");
    ASSERT(fc_gdb_stub_check_break(0x2000) == 1, "bp2 still present");
}

/* ── state / flow tests ──────────────────────────────────────────────────── */

static void test_pending_action_initial(void) {
    reset_stub();
    ASSERT(fc_gdb_stub_pending_action() == -1, "initial pending_action = -1");
}

static void test_pending_action_after_kill(void) {
    reset_stub();
    ASSERT_STR_EQ(drive("k"), "W00");
    ASSERT(fc_gdb_stub_pending_action() == 2, "kill → pending_action = 2");
}

static void test_detach(void) {
    reset_stub();
    ASSERT_STR_EQ(drive("D"), "OK");
    ASSERT(fc_gdb_stub_pending_action() == 0, "detach → pending_action = 0");
    ASSERT(fc_gdb_stub_is_halted() == 0, "detach → not halted");
}

static void test_interrupt(void) {
    reset_stub();
    /* \x03 is the out-of-band interrupt. */
    drive("\x03");
    ASSERT(fc_gdb_stub_is_halted() == 1, "interrupt → halted");
    ASSERT_STR_EQ(g_sent, "T02thread:01;");
}

static void test_vCont_step_sets_pending(void) {
    reset_stub();
    /* vCont;s is deferred (no immediate response), sets pending_action = 1. */
    drive("vCont;s");
    ASSERT(fc_gdb_stub_pending_action() == 1, "vCont;s → pending_action = 1");
    ASSERT(fc_gdb_stub_is_halted() == 0, "vCont;s → not halted");
}

/* ── qXfer tests ─────────────────────────────────────────────────────────── */

static void test_qXfer_exec_file(void) {
    reset_stub();
    fc_gdb_layout_t layout = {.exec_path = "/tmp/test.blyt", .libraries = NULL, .n_libraries = 0};
    fc_gdb_stub_set_layout(&layout);
    const char *r = drive("qXfer:exec-file:read::0,fff");
    ASSERT(r[0] == 'l', "prefix 'l' (complete)");
    ASSERT_CONTAINS(r, "/tmp/test.blyt");
}

static void test_qXfer_libraries_empty(void) {
    reset_stub();
    const char *r = drive("qXfer:libraries-svr4:read::0,4000");
    ASSERT_CONTAINS(r, "<library-list-svr4");
    ASSERT_CONTAINS(r, "</library-list-svr4>");
}

static void test_qXfer_libraries_one_entry(void) {
    reset_stub();
    static const fc_gdb_library_t libs[1] = {{
        .path = "libblyt32.so",
        .l_addr = 0x08000000,
        .l_ld = 0x08001000,
    }};
    fc_gdb_layout_t layout = {.exec_path = NULL, .libraries = libs, .n_libraries = 1};
    fc_gdb_stub_set_layout(&layout);
    const char *r = drive("qXfer:libraries-svr4:read::0,8000");
    ASSERT_CONTAINS(r, "libblyt32.so");
    ASSERT_CONTAINS(r, "0x8000000");
}

/* ── qProcessInfo / qRegisterInfo tests ─────────────────────────────────── */

static void test_qProcessInfo(void) {
    reset_stub();
    const char *r = drive("qProcessInfo");
    ASSERT_CONTAINS(r, "riscv32");
    ASSERT_CONTAINS(r, "endian:little");
    ASSERT_CONTAINS(r, "ptrsize:04");
}

static void test_qRegisterInfo_x0(void) {
    reset_stub();
    const char *r = drive("qRegisterInfo0");
    ASSERT_CONTAINS(r, "name:x0");
    ASSERT_CONTAINS(r, "bitsize:32");
    ASSERT_CONTAINS(r, "dwarf:0");
}

static void test_qRegisterInfo_pc(void) {
    reset_stub();
    /* PC = register 32 = 0x20 hex. */
    const char *r = drive("qRegisterInfo20");
    ASSERT_CONTAINS(r, "name:pc");
    ASSERT_CONTAINS(r, "generic:pc");
    ASSERT_CONTAINS(r, "dwarf:65");
}

static void test_qRegisterInfo_end(void) {
    reset_stub();
    /* Register 33 = 0x21 hex → past end → E45. */
    ASSERT_STR_EQ(drive("qRegisterInfo21"), "E45");
}

/* ── P (single register write) tests ────────────────────────────────────── */

static void test_P_write_register(void) {
    reset_stub();
    /* Write x1 (ra) = 0x1234ABCD via P1:cdab3412 (little-endian). */
    ASSERT_STR_EQ(drive("P1:cdab3412"), "OK");
    /* Read back with p1 and verify. */
    ASSERT_STR_EQ(drive("p1"), "cdab3412");
    /* Verify g_regs were updated. */
    ASSERT(g_regs[4] == 0xCD, "byte 0 = 0xCD");
    ASSERT(g_regs[5] == 0xAB, "byte 1 = 0xAB");
    ASSERT(g_regs[6] == 0x34, "byte 2 = 0x34");
    ASSERT(g_regs[7] == 0x12, "byte 3 = 0x12");
}

static void test_P_out_of_range(void) {
    reset_stub();
    /* Register 33 = 0x21 hex → beyond end. */
    ASSERT_STR_EQ(drive("P21:00000000"), "E00");
}

/* ── qXfer:features:read test ────────────────────────────────────────────── */

static void test_qXfer_features_read(void) {
    reset_stub();
    const char *r = drive("qXfer:features:read:target.xml:0,4000");
    ASSERT(r[0] == 'l', "prefix 'l' (complete)");
    ASSERT_CONTAINS(r, "riscv");
}

static void test_qXfer_features_unknown_annex(void) {
    reset_stub();
    ASSERT_STR_EQ(drive("qXfer:features:read:unknown.xml:0,4000"), "E01");
}

/* ── T (thread alive) test ───────────────────────────────────────────────── */

static void test_T_thread_alive(void) {
    reset_stub();
    ASSERT_STR_EQ(drive("T1"), "OK"); /* thread 1 is alive */
    ASSERT_STR_EQ(drive("T2"), "E01"); /* thread 2 doesn't exist */
    ASSERT_STR_EQ(drive("T01"), "OK"); /* leading zero */
}

/* ── vCont;S uppercase step test ─────────────────────────────────────────── */

static void test_vCont_S_uppercase_step(void) {
    reset_stub();
    /* vCont;S:05 is deferred — no immediate response; pending_action = 1 (step). */
    drive("vCont;S:05");
    ASSERT(fc_gdb_stub_pending_action() == 1, "vCont;S → pending_action = 1 (step)");
    ASSERT_STR_EQ(g_sent, "");
}

/* ── Z0 overflow test ────────────────────────────────────────────────────── */

static void test_Z0_overflow(void) {
    reset_stub();
    char pkt[32];
    int i;
    for (i = 0; i < 128; i++) {
        snprintf(pkt, sizeof pkt, "Z0,%x,4", 0x10000 + i * 4);
        ASSERT_STR_EQ(drive(pkt), "OK");
    }
    /* One more beyond MAX_BREAKS (128) → error. */
    snprintf(pkt, sizeof pkt, "Z0,%x,4", 0x10000 + 128 * 4);
    ASSERT_STR_EQ(drive(pkt), "E01");
}

/* ── Hardware breakpoint unsupported test ───────────────────────────────── */

static void test_z1_hardware_bp_unsupported(void) {
    reset_stub();
    /* Z1 (hardware breakpoint) — not supported; expect empty response. */
    ASSERT_STR_EQ(drive("Z1,10000,4"), "");
    ASSERT_STR_EQ(drive("Z2,10000,4"), "");
    ASSERT_STR_EQ(drive("z1,10000,4"), "");
}

/* ── qThreadStopInfo tests ───────────────────────────────────────────────── */

static void test_qThreadStopInfo_halted(void) {
    reset_stub();
    /* Halt the stub via \x03 interrupt. */
    drive("\x03");
    ASSERT(fc_gdb_stub_is_halted() == 1, "halted after interrupt");
    /* qThreadStopInfo while halted → T05 stop reason. */
    const char *r = drive("qThreadStopInfo1");
    ASSERT_CONTAINS(r, "T05");
    ASSERT_CONTAINS(r, "thread:01");
}

static void test_qThreadStopInfo_running(void) {
    reset_stub();
    /* set_transport() initialises halted=1; send vCont;c to move to running. */
    drive("vCont;c");
    ASSERT(fc_gdb_stub_is_halted() == 0, "running after vCont;c");
    ASSERT_STR_EQ(drive("qThreadStopInfo1"), "T00:;");
}

/* ── send_stop_with_regs tests ───────────────────────────────────────────── */

static void test_question_mark_includes_registers(void) {
    reset_stub();
    /* x1 = 0xDEADBEEF little-endian → bytes {0xDE, 0xAD, 0xBE, 0xEF} */
    g_regs[1 * 4 + 0] = 0xDE;
    g_regs[1 * 4 + 1] = 0xAD;
    g_regs[1 * 4 + 2] = 0xBE;
    g_regs[1 * 4 + 3] = 0xEF;
    /* PC (reg 32) = 0x00004000 little-endian → bytes {0x00, 0x40, 0x00, 0x00} */
    g_regs[32 * 4 + 0] = 0x00;
    g_regs[32 * 4 + 1] = 0x40;
    g_regs[32 * 4 + 2] = 0x00;
    g_regs[32 * 4 + 3] = 0x00;
    const char *r = drive("?");
    ASSERT_CONTAINS(r, "T05swbreak:;thread:01;");
    ASSERT_CONTAINS(r, "00:00000000;"); /* x0 always zero */
    ASSERT_CONTAINS(r, "01:deadbeef;"); /* x1 we set */
    ASSERT_CONTAINS(r, "20:00400000;"); /* PC we set */
}

static void test_notify_stopped_includes_registers(void) {
    reset_stub();
    /* x2 (sp) = 0x12345678 little-endian → bytes {0x78, 0x56, 0x34, 0x12} */
    g_regs[2 * 4 + 0] = 0x78;
    g_regs[2 * 4 + 1] = 0x56;
    g_regs[2 * 4 + 2] = 0x34;
    g_regs[2 * 4 + 3] = 0x12;
    g_sent[0] = '\0';
    fc_gdb_stub_notify_stopped();
    ASSERT_CONTAINS(g_sent, "T05swbreak:;thread:01;");
    ASSERT_CONTAINS(g_sent, "02:78563412;");
    ASSERT(fc_gdb_stub_is_halted() == 1, "halted after notify_stopped");
}

static void test_qThreadStopInfo_includes_registers(void) {
    reset_stub();
    drive("\x03"); /* halt */
    ASSERT(fc_gdb_stub_is_halted() == 1, "halted after interrupt");
    /* x3 = 0xCAFEBABE little-endian → bytes {0xBE, 0xBA, 0xFE, 0xCA} */
    g_regs[3 * 4 + 0] = 0xBE;
    g_regs[3 * 4 + 1] = 0xBA;
    g_regs[3 * 4 + 2] = 0xFE;
    g_regs[3 * 4 + 3] = 0xCA;
    const char *r = drive("qThreadStopInfo1");
    ASSERT_CONTAINS(r, "T05swbreak:;thread:01;");
    ASSERT_CONTAINS(r, "03:bebafeca;");
}

/* ── disconnect / reconnect tests ───────────────────────────────────────── */

static void test_disconnect_clears_breakpoints(void) {
    reset_stub();
    drive("Z0,10000,4");
    drive("Z0,20000,4");
    ASSERT(fc_gdb_stub_check_break(0x10000) == 1, "bp1 before disconnect");
    fc_gdb_stub_set_has_client(0);
    ASSERT(fc_gdb_stub_check_break(0x10000) == 0, "bp1 cleared after disconnect");
    ASSERT(fc_gdb_stub_check_break(0x20000) == 0, "bp2 cleared after disconnect");
    ASSERT(g_bp_clear_count == 2, "clear_breakpoint called for each bp");
}

static void test_disconnect_resumes(void) {
    reset_stub();
    drive("\x03");
    ASSERT(fc_gdb_stub_is_halted() == 1, "halted before disconnect");
    ASSERT(fc_gdb_stub_pending_action() == -1, "pending = -1 before disconnect");
    fc_gdb_stub_set_has_client(0);
    ASSERT(fc_gdb_stub_is_halted() == 0, "not halted after disconnect");
    ASSERT(fc_gdb_stub_pending_action() == 0, "pending = 0 (continue) after disconnect");
}

static void test_reconnect_reinitializes(void) {
    reset_stub();
    drive("vCont;c");
    ASSERT(fc_gdb_stub_is_halted() == 0, "running after vCont;c");
    fc_gdb_stub_set_has_client(0);
    /* New client connects. */
    fc_gdb_stub_set_has_client(1);
    ASSERT(fc_gdb_stub_is_halted() == 1, "halted after reconnect");
    ASSERT(fc_gdb_stub_pending_action() == -1, "pending = -1 after reconnect");
}

/* ── initial-halt clearing ───────────────────────────────────────────────── */

static void test_continue_initial_halt_clears_startup_halt(void) {
    reset_stub();
    /* set_transport parks the stub in the startup halt. */
    ASSERT(fc_gdb_stub_is_halted() == 1, "startup -> halted");
    fc_gdb_stub_continue_initial_halt();
    ASSERT(fc_gdb_stub_is_halted() == 0, "initial halt cleared");
    ASSERT(fc_gdb_stub_pending_action() == 0, "released to continue");
}

static void test_continue_initial_halt_leaves_breakpoint_halt(void) {
    reset_stub();
    fc_gdb_stub_continue_initial_halt(); /* clear the startup halt */
    /* A breakpoint/step stop parks at halted=1 / pending_action=-1 — the
     * same shape as the startup halt.  It must NOT be released here
     * (regression: the WASM trampoline pause called this each tick and
     * silently resumed the guest past a hit breakpoint). */
    fc_gdb_stub_notify_stopped();
    ASSERT(fc_gdb_stub_is_halted() == 1, "breakpoint -> halted");
    fc_gdb_stub_continue_initial_halt();
    ASSERT(fc_gdb_stub_is_halted() == 1, "breakpoint halt NOT cleared");
    ASSERT(fc_gdb_stub_pending_action() == -1, "pending_action unchanged");
}

static void test_continue_initial_halt_leaves_interrupt_halt(void) {
    reset_stub();
    fc_gdb_stub_continue_initial_halt();
    drive("\x03");
    ASSERT(fc_gdb_stub_is_halted() == 1, "interrupt -> halted");
    fc_gdb_stub_continue_initial_halt();
    ASSERT(fc_gdb_stub_is_halted() == 1, "interrupt halt NOT cleared");
}

static void test_continue_initial_halt_after_reconnect(void) {
    reset_stub();
    fc_gdb_stub_continue_initial_halt();
    fc_gdb_stub_set_has_client(0);
    fc_gdb_stub_set_has_client(1);
    /* Reconnect re-arms the startup halt; clearing it again is allowed. */
    fc_gdb_stub_continue_initial_halt();
    ASSERT(fc_gdb_stub_is_halted() == 0, "reconnect halt cleared");
}

/* ── main ────────────────────────────────────────────────────────────────── */

#define RUN(fn)                                                                                    \
    do {                                                                                           \
        fn();                                                                                      \
        printf("  %s\n", #fn);                                                                     \
    } while (0)

int main(void) {
    printf("test_gdb_stub: running\n");

    RUN(test_qSupported);
    RUN(test_qAttached);
    RUN(test_qC);
    RUN(test_vContQ);
    RUN(test_qfThreadInfo);
    RUN(test_H);
    RUN(test_QStartNoAckMode);
    RUN(test_QThreadSuffixSupported);
    RUN(test_question_mark);
    RUN(test_unknown_packet);

    RUN(test_g_register_read);
    RUN(test_p_single_register);
    RUN(test_p_pc_register);
    RUN(test_p_out_of_range);
    RUN(test_G_write_registers);

    RUN(test_m_read);
    RUN(test_m_read_returns_empty_for_unmapped);
    RUN(test_M_write);

    RUN(test_Z0_set_and_check);
    RUN(test_check_break_empty_table);
    RUN(test_Z0_multiple);
    RUN(test_z0_remove);

    RUN(test_pending_action_initial);
    RUN(test_pending_action_after_kill);
    RUN(test_detach);
    RUN(test_interrupt);
    RUN(test_vCont_step_sets_pending);

    RUN(test_qXfer_exec_file);
    RUN(test_qXfer_libraries_empty);
    RUN(test_qXfer_libraries_one_entry);

    RUN(test_qProcessInfo);
    RUN(test_qRegisterInfo_x0);
    RUN(test_qRegisterInfo_pc);
    RUN(test_qRegisterInfo_end);

    RUN(test_P_write_register);
    RUN(test_P_out_of_range);
    RUN(test_qXfer_features_read);
    RUN(test_qXfer_features_unknown_annex);
    RUN(test_T_thread_alive);
    RUN(test_vCont_S_uppercase_step);
    RUN(test_Z0_overflow);
    RUN(test_z1_hardware_bp_unsupported);
    RUN(test_qThreadStopInfo_halted);
    RUN(test_qThreadStopInfo_running);

    RUN(test_question_mark_includes_registers);
    RUN(test_notify_stopped_includes_registers);
    RUN(test_qThreadStopInfo_includes_registers);

    RUN(test_disconnect_clears_breakpoints);
    RUN(test_disconnect_resumes);
    RUN(test_reconnect_reinitializes);

    RUN(test_continue_initial_halt_clears_startup_halt);
    RUN(test_continue_initial_halt_leaves_breakpoint_halt);
    RUN(test_continue_initial_halt_leaves_interrupt_halt);
    RUN(test_continue_initial_halt_after_reconnect);

    if (failures == 0) {
        printf("test_gdb_stub: all %d tests passed\n", 50);
        return 0;
    }
    fprintf(stderr, "test_gdb_stub: %d test(s) FAILED\n", failures);
    return 1;
}
