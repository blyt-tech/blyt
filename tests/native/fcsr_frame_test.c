/*
 * fcsr_frame_test.c — validates the FCSR frame-boundary check.
 *
 * Self-contained RV32 ILP32F binary (no libc). Cross-compiled on the host:
 *   ${BLYT_RV32_CLANG} --target=riscv32-linux-gnu -march=rv32imafc -mabi=ilp32f \
 *       -O2 -nostdlib -o fcsr_debug_test fcsr_frame_test.c
 *   ${BLYT_RV32_CLANG} ... -DNDEBUG -o fcsr_release_test fcsr_frame_test.c
 *
 * Expected exit codes:
 *   fcsr_debug_test  : 0  (warning printed to stderr, execution continued)
 *   fcsr_release_test: 1  (abort message to stderr, process exits)
 */

/* SYS_write = 64, SYS_exit_group = 94 */
static inline void rs_write(int fd, const char *buf, unsigned int len) {
    register long _a0 __asm__("a0") = fd;
    register const char *_a1 __asm__("a1") = buf;
    register long _a2 __asm__("a2") = len;
    register long _a7 __asm__("a7") = 64;
    __asm__ volatile("ecall" : "+r"(_a0) : "r"(_a7), "r"(_a1), "r"(_a2) : "memory");
}

static __attribute__((noreturn)) void rs_exit_group(int code) {
    register long _a0 __asm__("a0") = code;
    register long _a7 __asm__("a7") = 94;
    __asm__ volatile("ecall" : : "r"(_a0), "r"(_a7) : "memory");
    __builtin_unreachable();
}

/* Mirrors blyt_frame_done() in frontends/native/src/libblyt32/blyt32.c. */
static void frame_boundary_check(void) {
    unsigned int fcsr;
    __asm__ volatile("csrr %0, fcsr" : "=r"(fcsr));
    unsigned int frm = (fcsr >> 5) & 0x7u;
    if (frm != 0u) {
#ifndef NDEBUG
        static const char pfx[] = "blyt: WARNING: cart set non-default FP rounding mode (frm=";
        static const char sfx[] = "); results may be non-deterministic\n";
        char digit = (char)('0' + frm);
        rs_write(2, pfx, sizeof(pfx) - 1);
        rs_write(2, &digit, 1);
        rs_write(2, sfx, sizeof(sfx) - 1);
#else
        static const char msg[] = "blyt: cart set non-default FP rounding mode; "
                                  "aborting for determinism\n";
        rs_write(2, msg, sizeof(msg) - 1);
        rs_exit_group(1);
#endif
    }
    __asm__ volatile("csrw fcsr, zero" ::: "memory");
}

static int test_main(void) {
    /* Verify clean FCSR produces no warning. */
    __asm__ volatile("csrw fcsr, zero" ::: "memory");
    frame_boundary_check();

    /* Set frm=3 (round toward +infinity) and verify the check fires. */
    __asm__ volatile("csrw frm, %0" ::"r"(3u));
    frame_boundary_check();
    /* debug: warning printed above, execution continues here */
    /* release: exited above with code 1 */

    /* Verify FCSR was reset to 0 by the debug check. */
    unsigned int fcsr_after;
    __asm__ volatile("csrr %0, fcsr" : "=r"(fcsr_after));
    if (fcsr_after != 0u) {
        static const char msg[] = "fcsr_frame_test: FCSR not reset\n";
        rs_write(2, msg, sizeof(msg) - 1);
        return 1;
    }
    return 0;
}

__attribute__((noreturn)) void _start(void) {
    rs_exit_group(test_main());
}
