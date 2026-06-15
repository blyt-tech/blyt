/*
 * seccomp_restricted_test.c — validates the restricted seccomp filter.
 *
 * Self-contained RV32 ILP32F binary (no libc). Cross-compiled on the host:
 *   ${BLYT_RV32_CLANG} --target=riscv32-linux-gnu -march=rv32imafc -mabi=ilp32f \
 *       -O2 -nostdlib -I <repo>/frontends/native/src/libblyt32 \
 *       -o seccomp_restricted_test seccomp_restricted_test.c
 *
 * Expected exit code: 159 (killed by SIGSYS from seccomp).
 */
#include "seccomp_restricted.h"

/* SYS_prctl = 167 */
static inline int rs_prctl(int op, long a1, long a2, long a3, long a4) {
    register long _a0 __asm__("a0") = op;
    register long _a1 __asm__("a1") = a1;
    register long _a2 __asm__("a2") = a2;
    register long _a3 __asm__("a3") = a3;
    register long _a4 __asm__("a4") = a4;
    register long _a7 __asm__("a7") = 167;
    __asm__ volatile("ecall"
                     : "+r"(_a0)
                     : "r"(_a7), "r"(_a1), "r"(_a2), "r"(_a3), "r"(_a4)
                     : "memory");
    return (int)_a0;
}

/* SYS_socket = 198 */
static inline int rs_socket(int domain, int type, int proto) {
    register long _a0 __asm__("a0") = domain;
    register long _a1 __asm__("a1") = type;
    register long _a2 __asm__("a2") = proto;
    register long _a7 __asm__("a7") = 198;
    __asm__ volatile("ecall" : "+r"(_a0) : "r"(_a7), "r"(_a1), "r"(_a2) : "memory");
    return (int)_a0;
}

#define PR_SET_NO_NEW_PRIVS 38
#define AF_INET 2
#define SOCK_STREAM 1

static int test_main(void) {
    if (rs_prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
        return 1; /* prctl failed */
    if (blyt_install_restricted_filter() != 0)
        return 2; /* filter install failed */
    /* AF_INET=2, SOCK_STREAM=1 — SYS_socket=198, not in restricted allowlist */
    rs_socket(AF_INET, SOCK_STREAM, 0);
    /* Should not reach here: SIGSYS kills the process before socket returns */
    return 3;
}

__attribute__((noreturn)) void _start(void) {
    blyt_rs_exit_group(test_main());
}
