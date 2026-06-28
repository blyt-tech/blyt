/*
 * blytcommon_emu.c — emulated-path impls of the variant-agnostic lifecycle/IO
 * APIs (issue #128).
 *
 * The emulated counterpart to frontends/native/src/libblytcommon/blytcommon.c:
 * the "common API, variant-specific impl" symbols whose emulated form is an
 * ECALL the host services (frame_done, console_debug, exit) or a no-op
 * (runtime_startup).  They live in libblytcommon — not the libblyt32 variant —
 * because the API is variant-agnostic; only the mechanism (ECALL vs native
 * syscall / FCSR / seccomp) differs per variant.
 *
 * The portable lifecycle driver (blyt_main) is in blyt_common.c, compiled into
 * the same library.  The host-backed data-transport stubs (state buffers,
 * save/load, resources) remain in the libblyt32 variant: those delegate to a
 * host-side implementation over the ECALL boundary, so the guest side is
 * genuinely variant transport (and #129/#123 rework them).
 */

#include "blyt.h"

#include "blyt_mem_budget.h" /* runtime/shared: blyt_mem_accounting_t + budget cap */

/* ECALL numbers (must match runtime/host/src/libblyt/ecall.h). */
#define ECALL_CONSOLE_DEBUG 1
#define ECALL_FRAME_DONE 2
#define ECALL_RESOURCE_PIN 61
#define ECALL_RESOURCE_UNPIN 62
#define ECALL_RESOURCE_LOAD 63
#define ECALL_RESOURCE_RELEASE 64
#define ECALL_MEM_RESOURCES 70

static unsigned int blytcommon_strlen(const char *s) {
    const char *p = s;
    while (*p)
        p++;
    return (unsigned int)(p - s);
}

/* blyt_frame_done — end-of-frame signal (ECALL 2).
 *
 * Called by blyt_main after each blyt_cart_draw().  The host intercepts this
 * ECALL, runs its frame callback (SDL event polling, frame-rate cap, etc.),
 * then resumes the emulator for the next frame without halting it. */
void blyt_frame_done(void) {
    register long a7 __asm__("a7") = ECALL_FRAME_DONE;
    __asm__ volatile("ecall" : : "r"(a7) : "memory");
}

/* blyt_console_debug — ADR-0085 ECALL stub (a0=ptr, a1=len). */
void blyt_console_debug(const char *s) {
    register const char *a0 __asm__("a0") = s;
    register unsigned int a1 __asm__("a1") = blytcommon_strlen(s);
    register long a7 __asm__("a7") = ECALL_CONSOLE_DEBUG;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a7) : "memory");
}

/* blyt_exit — clean process exit.
 *
 * Called by _blyt_entry after blyt_main() returns.  exit_group(2) (NR 94) is
 * the same mechanism on both paths — on the emulated path the emulator
 * intercepts the ECALL and halts the simulation; on native the kernel handles
 * it — so this is variant-agnostic.  In practice ECALL_QUIT usually halts the
 * emulator first, so blyt_exit is rarely reached on the emulated path. */
__attribute__((noreturn)) void blyt_exit(int code) {
    register long a0 __asm__("a0") = code;
    register long a7 __asm__("a7") = 94; /* SYS_exit_group */
    __asm__ volatile("ecall" : : "r"(a0), "r"(a7));
    __builtin_unreachable();
}

/* blyt_runtime_startup — no-op on emulated targets.  The native libblytcommon
 * variant installs the restricted seccomp filter and resets FCSR before cart
 * code runs; on the emulated path the host owns that setup, so there is nothing
 * to do here. */
void blyt_runtime_startup(void) {
}

/* ── Resource lifecycle ECALL stubs (ADR-0027, #123) ────────────────────────
 *
 * Emulated-path stubs: the host services pin/unpin/load/release over the ECALL
 * boundary (runtime/host/src/libblyt/cart_run.c) against its resource table.
 * The native libblytcommon variant overrides these with real implementations.
 * The blyt_resource_text_get convenience is portable (resources.c) and built on
 * top of pin/unpin, so it is not duplicated here. */

blyt_result_t blyt_resource_pin(blyt_resource_id_t id, const void **out_ptr, size_t *out_size) {
    register long a0 __asm__("a0") = (long)id;
    register long a1 __asm__("a1") = (long)out_ptr;
    register long a2 __asm__("a2") = (long)out_size;
    register long a7 __asm__("a7") = ECALL_RESOURCE_PIN;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return (blyt_result_t)a0;
}

blyt_result_t blyt_resource_unpin(blyt_resource_id_t id) {
    register long a0 __asm__("a0") = (long)id;
    register long a7 __asm__("a7") = ECALL_RESOURCE_UNPIN;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a7) : "memory");
    return (blyt_result_t)a0;
}

blyt_result_t blyt_resource_load(blyt_resource_id_t id, blyt_resource_h *out_handle) {
    register long a0 __asm__("a0") = (long)id;
    register long a1 __asm__("a1") = (long)out_handle;
    register long a7 __asm__("a7") = ECALL_RESOURCE_LOAD;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a7) : "memory");
    return (blyt_result_t)a0;
}

blyt_result_t blyt_resource_release(blyt_resource_h handle) {
    register long a0 __asm__("a0") = (long)handle;
    register long a7 __asm__("a7") = ECALL_RESOURCE_RELEASE;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a7) : "memory");
    return (blyt_result_t)a0;
}

/* ── Memory introspection (ADR-0029, #159) ──────────────────────────────────
 *
 * The scalar totals are published sums in the guest-readable accounting block
 * (blyt_mem_acct, exported by libblytc, #158): blyt_mem_stats reads them with NO
 * ECALL — the "running totals" ADR-0029 calls for. Only the variable-length
 * loaded-resource list needs the host, resolved on demand via ECALL. The native
 * libblytcommon variant overrides both with direct implementations. */
extern blyt_mem_accounting_t blyt_mem_acct;

void blyt_mem_stats(blyt_mem_stats_t *out) {
    if (!out)
        return;
    uint32_t heap = blyt_mem_acct.guest_heap_used;
    uint32_t cache = blyt_mem_acct.resource_cache_used;
    out->resource_cache_used = cache;
    out->cart_allocations = heap;
    out->total_used = heap + cache;
    out->budget_cap = BLYT_MEM_BUDGET_BYTES;
}

uint32_t blyt_mem_resources(blyt_mem_resource_t *resources, uint32_t cap) {
    register long a0 __asm__("a0") = (long)resources;
    register long a1 __asm__("a1") = (long)cap;
    register long a7 __asm__("a7") = ECALL_MEM_RESOURCES;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a7) : "memory");
    return (uint32_t)a0;
}
