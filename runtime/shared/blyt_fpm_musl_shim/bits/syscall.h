/*
 * runtime/shared/blyt_fpm_musl_shim/bits/syscall.h — Phase B header shim
 * (ADR-0135, blyt#225).
 *
 * <sys/syscall.h> includes <bits/syscall.h> (normally a generated per-arch table
 * of SYS_* numbers). The vendored musl stdio subset never issues a syscall, so
 * the SYS_* numbers are never referenced; provide an empty table so
 * <stdio_impl.h> compiles without needing the arch-generated header.
 */
#ifndef _BLYT_FPM_BITS_SYSCALL_H
#define _BLYT_FPM_BITS_SYSCALL_H
/* intentionally empty — the seam performs no syscalls */
#endif
