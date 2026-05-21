/*
 * fcsr_frame_test.c — validates the FCSR frame-boundary check.
 *
 * The check logic is inlined here (not via libblyt32.so) so that -DNDEBUG at
 * compile time independently controls the debug/release path, regardless of
 * how libblyt32.so was built.  This lets both behaviors be exercised in a
 * single QEMU session.
 *
 * Build on the QEMU target (or via cross-compiler):
 *   riscv32-linux-musl-gcc -o fcsr_debug_test   fcsr_frame_test.c
 *   riscv32-linux-musl-gcc -DNDEBUG -o fcsr_release_test fcsr_frame_test.c
 *
 * Expected exit codes:
 *   fcsr_debug_test  : 0  (warning printed to stderr, execution continued)
 *   fcsr_release_test: 1  (abort message printed to stderr, process exits)
 */

#include <stdio.h>
#include <stdlib.h>

/*
 * frame_boundary_check — mirrors blyt_frame_done() in libblyt32.so.
 *
 * Reads FCSR, extracts frm (bits 7:5).  If non-zero:
 *   debug  : prints a warning to stderr, continues
 *   release: prints an abort message to stderr, exits 1
 * Always resets FCSR to 0 in the debug path.
 */
static void frame_boundary_check(void) {
    unsigned int fcsr;
    __asm__ volatile("csrr %0, fcsr" : "=r"(fcsr));
    unsigned int frm = (fcsr >> 5) & 0x7u;
    if (frm != 0u) {
#ifndef NDEBUG
        fprintf(stderr,
                "blyt: WARNING: cart set non-default FP rounding mode "
                "(frm=%u); results may be non-deterministic\n",
                frm);
#else
        fprintf(stderr, "blyt: cart set non-default FP rounding mode; "
                        "aborting for determinism\n");
        exit(1);
#endif
    }
    __asm__ volatile("csrw fcsr, zero" ::: "memory");
}

int main(void) {
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
        fprintf(stderr, "fcsr_frame_test: FCSR not reset (got 0x%x)\n", fcsr_after);
        return 1;
    }

    return 0;
}
