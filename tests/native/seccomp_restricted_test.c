/*
 * seccomp_restricted_test.c — validates the restricted seccomp filter.
 *
 * Compiled as an ILP32 RISCV binary (NOT a blyt cart) using the musl ILP32
 * toolchain.  Installs the restricted seccomp filter then attempts socket(2)
 * which must be blocked → SIGSYS → exit code 159 (128 + SIGSYS=31).
 *
 * Build on the QEMU target (or via cross-compiler):
 *   riscv32-linux-musl-gcc \
 *       -I<repo>/frontends/native/src/libblyt32 \
 *       -o seccomp_restricted_test seccomp_restricted_test.c
 *
 * Expected exit code: 159 (killed by SIGSYS from seccomp).
 * If it exits 0: the filter was not installed or did not block socket.
 * If it exits 1: prctl failed.
 * If it exits 2: filter install failed.
 * If it exits 3: socket() returned without SIGSYS (should never happen).
 */

#include "seccomp_restricted.h"
#include <sys/prctl.h>
#include <sys/socket.h>

int main(void) {
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
        return 1; /* prctl failed */

    if (blyt_install_restricted_filter() != 0)
        return 2; /* filter install failed */

    /* AF_INET=2, SOCK_STREAM=1, proto=0 — NR 198, not in restricted allowlist */
    socket(AF_INET, SOCK_STREAM, 0);

    /* Should not reach here: SIGSYS kills the process before socket returns */
    return 3;
}
