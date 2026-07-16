/*
 * blyt_hostlua_driver.h — the ONE cart-driving execution model for the host-Lua
 * fast path (#242, epic #230, ADR-0136).
 *
 * WHY THIS FILE EXISTS
 * --------------------
 * `guest_heap_used` (blyt32.mem.stats().cart_allocations) must be byte-identical
 * on every host-Lua leg (ADR-0029/0008). The #231 seam normalises object SIZES
 * down to the rv32 canonical, which closed the per-object gap — but a bounded
 * residual survived, because the two legs executed the cart through two
 * different runners:
 *
 *   - wasm  (frontends/wasm/wasm_main.c) drove the cart through a `co_body`
 *     coroutine resumed once per frame by a C phase machine;
 *   - native (runtime/host/src/libblyt/cart_run_hostlua.c) called
 *     init()/update()/draw() directly from C via lua_pcall.
 *
 * Those two interned DIFFERENT scaffolding strings, so the global string table
 * (`g->strt`) rehashed at different counts, shifting every allocation-sequence-
 * sensitive construct (short-string interning, luaL_Buffer-boxed strings,
 * closures with open upvalues, coroutine threads) by tens–hundreds of bytes.
 *
 * The fix is not to mirror the two runners more carefully — that contract had
 * already failed once (#235: the S-proxy type table silently fell out of
 * lockstep and misrouted f64 to i32). It is to make the execution model a
 * SINGLE source of truth that both legs compile. With one set of chunk strings
 * driving one coroutine shape, the allocation sequence is identical BY
 * CONSTRUCTION rather than by hand-maintained agreement, and there is nothing
 * left to diverge.
 *
 * THE COROUTINE IS ONLY A CONTAINER
 * ---------------------------------
 * Converging on the coroutine does NOT make native yield for debugging. The
 * wasm leg needs a coroutine because the single-threaded browser/Node event
 * loop cannot block — a breakpoint must lua_yield out (dap_transport_wasm.c,
 * ASYNCIFY). Native pauses by BLOCKING a thread instead: the DAP reader runs on
 * a background pthread and fc_dap_pause_loop() parks the execution thread on a
 * pthread_cond_wait inside the Lua hook (dap_transport_tcp_lua.c). Blocking
 * inside a coroutine is perfectly fine, so native keeps its thread-blocking
 * pause and the coroutine serves only as the shared execution container for
 * allocation-sequence parity.
 *
 * WHY THE CALLBACKS GO THROUGH __blyt_call
 * ----------------------------------------
 * The lifecycle calls are dispatched by a C closure (`__blyt_call`) that pcalls
 * the named global and reports a Lua error as the BARE message (#258), rather
 * than being called directly from the chunk. That is what makes an erroring
 * callback REPORT-AND-CONTINUE: the error is caught at the pcall inside the
 * coroutine, so the coroutine itself never unwinds and stays resumable.
 *
 * A bare `update()` in the chunk would kill the coroutine on the first error and
 * leave it unresumable — which is exactly the WASM teardown bug (#264), and
 * would equally have REGRESSED native, whose per-callback pcall recovery
 * (#236/#258) deliberately matches the guest's `call_global` on the emulated
 * path and real RISC-V hardware. Routing through __blyt_call gives every leg the
 * guest's semantics from one definition.
 */

#ifndef BLYT_SHARED_HOSTLUA_DRIVER_H
#define BLYT_SHARED_HOSTLUA_DRIVER_H

#include "lua.h" /* lua_State / lua_CFunction for the registration below */

/*
 * The name of the driver's dispatch global: `__blyt_call(name)` looks up global
 * `name`, returns quietly if it is not a function, otherwise pcalls it with no
 * args and — on error — reports the bare lua_tostring message through the leg's
 * log channel and returns normally (report-and-continue, never rethrow).
 *
 * Registered by blyt_hostlua_driver_register_globals() below, NOT by the runner:
 * see that function for why the driver owns this rather than documenting it as
 * an obligation.
 */
#define BLYT_HOSTLUA_CALL_FN "__blyt_call"

/*
 * Phase brackets (#205, #232 S4): a hybrid cart's emulated native half reads the
 * lifecycle phase off the session run-ctx to keep surface access draw()-only.
 * Registered as global C closures so the ONE chunk drives them on every leg;
 * they no-op for a session-less pure-Lua cart.
 */
#define BLYT_HOSTLUA_PHASE_UPDATE_FN "__blyt_phase_update"
#define BLYT_HOSTLUA_PHASE_DRAW_FN "__blyt_phase_draw"
#define BLYT_HOSTLUA_PHASE_NONE_FN "__blyt_phase_none"

/*
 * Register the driver's OWN four globals, in one canonical order, from one
 * definition (#267). The leg supplies the four C functions — their bodies are
 * necessarily leg-specific (each logs and reads its phase off its own run-ctx) —
 * but the names, the count and above all the ORDER live here.
 *
 * Why the order is load-bearing rather than cosmetic. These names are interned
 * short strings, so registering them allocates, and they land in the scaffolding
 * that guest_heap_used subtracts as its baseline (see the baseline capture in
 * each runner). Subtracting the baseline removes the scaffolding's BYTES but not
 * its LAYOUT: the arena is first-fit and charges a recycled block whole when the
 * remainder is too small to split (blyt_arena.c), so a different registration
 * order leaves a different free list, and the cart's own later allocations get
 * charged differently. That is not theoretical — native registered
 * __blyt_call first and wasm registered it last, which refracted into a constant
 * ±16 B cross-leg divergence in cart_allocations that survived #242's runner
 * unification and blocked ADR-0029's determinism contract (#267).
 *
 * So: the driver defines these names, and the driver registers them. A leg that
 * hand-rolls the sequence can silently drift out of order again; calling this
 * cannot.
 */
static inline void blyt_hostlua_driver_register_globals(lua_State *L, lua_CFunction call_fn,
                                                        lua_CFunction phase_update_fn,
                                                        lua_CFunction phase_draw_fn,
                                                        lua_CFunction phase_none_fn) {
    lua_pushcfunction(L, call_fn);
    lua_setglobal(L, BLYT_HOSTLUA_CALL_FN);
    lua_pushcfunction(L, phase_update_fn);
    lua_setglobal(L, BLYT_HOSTLUA_PHASE_UPDATE_FN);
    lua_pushcfunction(L, phase_draw_fn);
    lua_setglobal(L, BLYT_HOSTLUA_PHASE_DRAW_FN);
    lua_pushcfunction(L, phase_none_fn);
    lua_setglobal(L, BLYT_HOSTLUA_PHASE_NONE_FN);
}

/*
 * Boot chunk: the guest blyt_main's boot phase — init() then on_new_state().
 * Runs to completion (LUA_OK); the runner then builds the running coroutine.
 */
#define BLYT_HOSTLUA_CO_BODY_INIT "__blyt_call('init') __blyt_call('on_new_state')"

/*
 * Reload boot chunk (#170/#244): run ONLY init() on the rebuilt VM. A hot reload
 * PRESERVES state, so there is no on_new_state() — the runner restores the
 * snapshot and replays on_load_state(HOT_RELOAD) once init() completes.
 */
#define BLYT_HOSTLUA_CO_BODY_RELOAD_INIT "__blyt_call('init')"

/*
 * Frame chunk: the guest blyt_main loop. One resume == one frame, delimited by
 * the coroutine.yield() at the bottom.
 *
 * Quit ordering matches blyt_main's `while (!g_quit_requested)`: the condition is
 * tested only at the top, so a quit requested during update() still runs THIS
 * frame's draw(). on_quit()/cleanup() run in the tail once the loop exits, after
 * which the resume returns LUA_OK and the runner marks the cart done.
 */
#define BLYT_HOSTLUA_CO_BODY_RUNNING                                                               \
    "while not blyt.should_quit() do "                                                             \
    "  __blyt_phase_update() "                                                                     \
    "  __blyt_call('update') "                                                                     \
    "  __blyt_phase_draw() "                                                                       \
    "  __blyt_call('draw') "                                                                       \
    "  __blyt_phase_none() "                                                                       \
    "  coroutine.yield() "                                                                         \
    "end "                                                                                         \
    "__blyt_call('on_quit') "                                                                      \
    "__blyt_call('cleanup')"

#endif /* BLYT_SHARED_HOSTLUA_DRIVER_H */
