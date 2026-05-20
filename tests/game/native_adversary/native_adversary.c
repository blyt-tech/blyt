/*
 * native_adversary — Phase-6 security gate cart.
 *
 * Attempts SYS_socket (NR 198), which is NOT in the phase-2 seccomp allowlist.
 * The kernel should deliver SIGSYS, and the launcher reports exit code 159
 * (128 + SIGSYS=31).  Used to verify that the phase-2 filter correctly blocks
 * syscalls outside the allowlist.
 *
 * Building and running: see native_hello.c for the build recipe.
 *
 * Expected: no output, exit code 159 (killed by SIGSYS from seccomp).
 */

/* SYS_socket = 198 on Linux RISC-V (unified syscall table) */
#define SYS_SOCKET 198u
#define SYS_EXIT_GROUP 94u

void _blyt_entry(void) {
    /* Attempt socket(AF_UNSPEC=0, 0, 0) — blocked by phase-2 → SIGSYS */
    register long a0 __asm__("a0") = 0;
    register long a1 __asm__("a1") = 0;
    register long a2 __asm__("a2") = 0;
    register long a7 __asm__("a7") = SYS_SOCKET;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7));

    /* Should not reach here if seccomp is installed correctly.
     * Exit with a non-zero code so a missed block is detectable. */
    register long b0 __asm__("a0") = 1;
    register long b7 __asm__("a7") = SYS_EXIT_GROUP;
    __asm__ volatile("ecall" : : "r"(b0), "r"(b7));
    __builtin_unreachable();
}
