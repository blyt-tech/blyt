/* blyt_trace — always-compiled-in, runtime-toggleable diagnostic tracing.
 *
 * Enabled via the BLYT_TRACE environment variable: a comma-separated list of
 * channel names ("gdb,dap,lifecycle,api,frame") or "all".  Parsed lazily on
 * the first blyt_trace_enabled()/blyt_tracef() call; when disabled the cost
 * is one load + branch per call site.
 *
 * Output goes to stderr only (stdout carries cart smoke-test output that CI
 * asserts).  Line format:
 *
 *   [blyt:<chan>] f=<frame> t=<ms> <msg>
 *
 * where t is milliseconds since the first trace call (CLOCK_MONOTONIC) and
 * f is a global frame counter advanced by blyt_trace_frame_mark().  Each
 * line is emitted with a single fprintf so POSIX stdio per-call locking
 * keeps lines whole across the GDB/DAP server threads — no extra mutex.
 *
 * Payloads are capped at 512 bytes (truncation marked with a "...(+N)"
 * suffix); BLYT_TRACE_FULL=1 raises the cap to 4096.
 *
 * Conventions per channel:
 *   gdb / dap   — "recv <payload>" / "send <payload>", "client connected" /
 *                 "client disconnected"
 *   lifecycle   — "call <name>" / "ret <name> a0=<val>" for host-initiated
 *                 guest calls
 *   api         — "<name>(<args>) -> <result>", one typed line per ECALL
 *                 handled by the host (including Lua bridge ops)
 *   frame       — "start" / "end" at frame boundaries
 *
 * Documented limitation: the steady-state update/draw loop runs inside the
 * *guest* (libblytcommon's blyt_main loop), so the host only observes frame
 * boundaries there.  Per-frame "call update" / "call draw" lifecycle lines
 * appear only for host-initiated calls (WASM hybrid trampolines,
 * reset-every-frame cycles, check_guest_quit).
 *
 * Coverage by frontend:
 *   channel    | blytplay/blytdebug | WASM emulated | WASM host-Lua | libretro | native (QEMU)
 *   gdb        | yes                | yes           | yes           | yes      | no (no stub)
 *   dap        | yes                | yes           | yes           | yes      | yes (mh_tracef)
 *   api        | yes (typed)        | yes (typed)   | bridge only   | yes      | yes (native lines)
 *   frame      | yes                | yes           | yes           | yes      | no
 *   lifecycle  | host-initiated     | same          | C call sites  | same     | no
 *
 * The native (QEMU) legs use small self-contained helpers (mh_tracef in
 * master_hook_native.c, the trace helper in frontends/native libblyt32) that
 * read BLYT_TRACE themselves and mirror this line format — host trace code
 * cannot run inside the seccomp'd ILP32 cart process.
 */
#pragma once

#include <stdint.h>

enum {
    BLYT_TRACE_GDB = 1u << 0,
    BLYT_TRACE_DAP = 1u << 1,
    BLYT_TRACE_LIFECYCLE = 1u << 2,
    BLYT_TRACE_API = 1u << 3,
    BLYT_TRACE_FRAME = 1u << 4,
    /* Reserved bitmask slots (not yet implemented):
     * BLYT_TRACE_STATE = 1u << 5, BLYT_TRACE_INPUT = 1u << 6 */
};

/* Returns nonzero when `chan` is enabled.  Lazy-parses BLYT_TRACE once. */
int blyt_trace_enabled(uint32_t chan);

/* Emit one trace line on `chan` (no-op when the channel is disabled). */
void blyt_tracef(uint32_t chan, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

/* Advance the global f=<frame> counter shown on every subsequent line. */
void blyt_trace_frame_mark(uint32_t frame_no);
