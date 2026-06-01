/* GDB remote serial protocol stub — transport-agnostic.
 *
 * Speaks at the guest CPU level: register read/write, memory read/write,
 * software breakpoints (ebreak patches), single-step.  Reports pre-mapped
 * library load addresses via qXfer:libraries-svr4:read.
 *
 * The stub owns no sockets, threads, or file descriptors.  All I/O is
 * delegated to an fc_gdb_transport_t provided by the caller before the
 * first breakpoint is set.
 *
 * TCP transport (native): gdb_transport_tcp.c
 * WebSocket transport (WASM): gdb_transport_wasm.c
 */

#ifndef BLYT_GDB_STUB_H
#define BLYT_GDB_STUB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Transport interface ──────────────────────────────────────────────────── */

typedef struct {
    /* Send GDB RSP packet (payload only; framing added by transport). */
    int (*send_pkt)(const char *payload);
    /* Receive one GDB RSP packet payload into buf.
     * Returns payload length on success, -1 if no packet is available. */
    int (*recv_pkt)(char *buf, size_t cap);
    /* Called when the CPU halts at a breakpoint or step boundary.
     * May block until the client sends vCont (TCP) or return immediately (WASM). */
    void (*on_stop)(void);
} fc_gdb_transport_t;

/* ── Library layout ───────────────────────────────────────────────────────── */

typedef struct {
    const char *path; /* host filesystem path of the .so (or registered name) */
    uint32_t l_addr; /* load base in guest memory */
    uint32_t l_ld; /* runtime address of dynamic section */
} fc_gdb_library_t;

typedef struct {
    const char *exec_path; /* cart path for qXfer:exec-file:read */
    const fc_gdb_library_t *libraries;
    int n_libraries;
} fc_gdb_layout_t;

/* ── CPU operations ───────────────────────────────────────────────────────── */

typedef struct {
    /* Read/write the 32 RV32 GPRs + PC into a 33×4-byte little-endian buffer. */
    void (*read_regs)(uint8_t out[33 * 4]);
    void (*write_regs)(const uint8_t in[33 * 4]);
    /* Read/write guest memory.  Returns bytes actually transferred. */
    uint32_t (*read_mem)(uint32_t addr, uint8_t *dst, uint32_t n);
    uint32_t (*write_mem)(uint32_t addr, const uint8_t *src, uint32_t n);
    /* Patch / unpatch a software breakpoint (ebreak = 0x00100073). */
    int (*set_breakpoint)(uint32_t addr);
    int (*clear_breakpoint)(uint32_t addr);
} fc_gdb_cpu_ops_t;

/* ── Public API ───────────────────────────────────────────────────────────── */

/* Register the active transport before calling any other function. */
void fc_gdb_stub_set_transport(const fc_gdb_transport_t *t);

/* Set layout; called after dynlink so library addresses are known. */
void fc_gdb_stub_set_layout(const fc_gdb_layout_t *layout);

/* Set CPU operations; called after the rv32emu instance is created. */
void fc_gdb_stub_set_cpu_ops(const fc_gdb_cpu_ops_t *ops);

/* Called by the instruction loop before each dispatch.
 * Returns 1 if the loop should pause (breakpoint hit or single-step boundary). */
int fc_gdb_stub_check_break(uint32_t pc);

/* Notify the client that the CPU has stopped (sends T05swbreak:;).
 * Call this before invoking transport->on_stop(). */
void fc_gdb_stub_notify_stopped(void);

/* Process one pending packet without blocking.
 * For WASM: called each animation tick while paused.
 * For TCP: called from the background reader thread (recv_pkt blocks there). */
void fc_gdb_stub_poll(void);

/* Process an already-received packet payload (used by TCP transport thread
 * which reads from the socket directly rather than via recv_pkt callback). */
void fc_gdb_stub_process_pkt(const char *pkt);

/* Returns the pending action after a vCont is received:
 *   0 = continue, 1 = single-step, 2 = exit, -1 = no action yet (still halted). */
int fc_gdb_stub_pending_action(void);

/* Returns 1 if a GDB client is currently connected. */
int fc_gdb_stub_has_client(void);

/* Called by the transport when a client connects (val=1) or disconnects (val=0).
 * On disconnect: clears all breakpoints and resumes the cart so the run loop
 * is not permanently blocked waiting for a vCont that will never arrive.
 * On reconnect: resets to initial-stop state so the new client can handshake. */
void fc_gdb_stub_set_has_client(int val);

/* Returns 1 if the CPU is currently halted (breakpoint, step boundary, or
 * out-of-band interrupt).  Used by the WASM run loop to detect \x03 interrupts
 * while the cart is in the RUNNING state. */
int fc_gdb_stub_is_halted(void);

/* Call transport->on_stop() — blocks for TCP, no-op for WASM. */
void fc_gdb_stub_block_until_resume(void);

/* Temporarily restore the original instruction at addr (for stepping through
 * a software breakpoint) without removing addr from the breaks[] table.
 * Call fc_gdb_stub_repatch_bp() after rv_step() to re-install the ebreak. */
void fc_gdb_stub_restore_bp_temp(uint32_t addr);

/* Re-install the ebreak at addr if it is still in the breaks[] table. */
void fc_gdb_stub_repatch_bp(uint32_t addr);

#ifdef __cplusplus
}
#endif

#endif /* BLYT_GDB_STUB_H */
