#pragma once

/* blyt_runtime.h — host/frontend-facing API (not shipped in the cart SDK). */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* --- Cart loading -------------------------------------------------------- */

typedef enum blyt_cart_err {
    BLYT_CART_OK = 0,
    BLYT_CART_ERR_IO = 1, /* file I/O error */
    BLYT_CART_ERR_TOO_SMALL = 2, /* file too small to contain an ELF header */
    BLYT_CART_ERR_NOT_ELF = 3, /* bad ELF magic */
    BLYT_CART_ERR_BAD_CLASS = 4, /* not ELFCLASS32 */
    BLYT_CART_ERR_BAD_ENDIAN = 5, /* not little-endian */
    BLYT_CART_ERR_BAD_OSABI = 6, /* EI_OSABI != ELFOSABI_NONE */
    BLYT_CART_ERR_BAD_MACHINE = 7, /* e_machine != EM_RISCV */
    BLYT_CART_ERR_BAD_FLAGS = 8, /* e_flags != expected RVC|ILP32F */
    BLYT_CART_ERR_BAD_SHDR = 9, /* section header table out of bounds */
    BLYT_CART_ERR_DENIED_SECT = 10, /* denied ELF section (.init_array family) */
    BLYT_CART_ERR_BAD_NEEDED = 11, /* DT_NEEDED allowlist violation */
    BLYT_CART_ERR_NO_CART_INFO = 12, /* .cart.info section missing */
    BLYT_CART_ERR_BAD_PREAMBLE = 13, /* section preamble tag or version mismatch */
    BLYT_CART_ERR_BAD_CART_INFO = 14, /* .cart.info FlatBuffers parse error */
    BLYT_CART_ERR_BAD_CART_CONFIG = 15, /* .cart.config FlatBuffers parse error */
    BLYT_CART_ERR_API_VERSION = 16, /* api_version unsupported */
    BLYT_CART_ERR_BAD_SEGMENT = 17, /* segment layout violation */
    BLYT_CART_ERR_BAD_INTERP = 18, /* PT_INTERP absent or != /lib/ld-blyt.so.1 */
    BLYT_CART_ERR_NO_RELRO = 19, /* PT_GNU_RELRO absent */
    BLYT_CART_ERR_BAD_OPCODE = 20, /* ecall or ebreak found in executable segment */
    BLYT_CART_ERR_BAD_IMPORT = 21, /* imported symbol not on allowlist */
    BLYT_CART_ERR_MISSING_ENTRY = 22, /* required cart entry point absent */
    BLYT_CART_ERR_BAD_LAYOUTS = 23, /* .cart.layouts FlatBuffers parse error */
    BLYT_CART_ERR_BAD_ID = 24, /* .cart.info id missing or invalid */
    BLYT_CART_ERR_BAD_TITLE = 25, /* .cart.info title missing or invalid */
    BLYT_CART_ERR_BAD_VERSION = 26, /* .cart.info version missing or invalid */
} blyt_cart_err_t;

typedef struct blyt_cart blyt_cart_t;

/*
 * Open and validate a cart file at the given path.
 * On success, *out is set to a newly allocated blyt_cart_t; BLYT_CART_OK
 * is returned. On failure, *out is set to NULL and an error code is returned.
 */
blyt_cart_err_t blyt_cart_open(const char *path, blyt_cart_t **out);

/* Close a cart opened with blyt_cart_open and free all resources. */
void blyt_cart_close(blyt_cart_t *cart);

/*
 * Return non-zero if the cart exports at least one lifecycle callback
 * (blyt_cart_init, blyt_cart_update, blyt_cart_draw, blyt_cart_on_new_state,
 * blyt_cart_on_quit, blyt_cart_cleanup) as a cart-native symbol (i.e. the
 * symbol is defined in the cart itself, not delegated to a runtime library).
 * Scans the cart's .dynsym directly — no emulator allocation.
 */
int blyt_cart_has_native_lifecycle(const blyt_cart_t *cart);

/* True iff the cart carries a native half (a C/Rust/C++ half) — i.e. it defines
 * any non-local FUNC symbol beyond the Lua bootstrap (`_blyt_entry`,
 * `cart_lua_modules`).  Distinguishes a pure-Lua cart from a hybrid for the
 * host-Lua dispatch predicate, catching even an unexported native helper (kept in
 * `.dynsym` by the cart's `--export-dynamic` link). */
int blyt_cart_has_native_code(const blyt_cart_t *cart);

/*
 * Bitmask of lifecycle callbacks defined in native (.dynsym) or Lua (.cart.lua).
 * Bit 0=init, 1=update, 2=draw, 3=on_new_state, 4=on_save_state,
 * 5=on_load_state, 6=on_quit, 7=cleanup.
 * blyt_cart_lua_lifecycle_mask requires BLYT_HAVE_HOST_LUA; returns 0 otherwise.
 */
uint32_t blyt_cart_native_lifecycle_mask(const blyt_cart_t *cart);
uint32_t blyt_cart_lua_lifecycle_mask(const blyt_cart_t *cart);

/* Return non-zero if the cart has a .cart.layouts section. */
int blyt_cart_has_layouts(const blyt_cart_t *cart);

/*
 * Cart identity from the .cart.info manifest, validated at load time:
 * id (machine identifier: save directory name), title (human-readable),
 * version (cart semver string).  All non-NULL on a successfully opened cart;
 * valid while the cart is open.
 */
const char *blyt_cart_id(const blyt_cart_t *cart);
const char *blyt_cart_title(const blyt_cart_t *cart);
const char *blyt_cart_version(const blyt_cart_t *cart);

/* .cart.config `save_version` (ADR-0125): the integer stamped into the .blys
 * save header at write time. 0 when undeclared. */
uint32_t blyt_cart_save_version(const blyt_cart_t *cart);

/* .cart.config `default_palette` (issue #201): the built-in palette handle to
 * auto-load before init() (`palettes: default:` in blyt.config.yaml). 0 when
 * undeclared -- the session resolves that to the runtime default (aurora). */
uint32_t blyt_cart_default_palette(const blyt_cart_t *cart);

/* Runtime version string (from version.txt, baked in at build time). */
const char *blyt_runtime_version(void);

/* Return a static human-readable string for a blyt_cart_err_t value. */
const char *blyt_cart_err_str(blyt_cart_err_t err);

/*
 * Find an ELF section by name in a validated cart.
 * Returns a pointer into the cart's mmap and sets *size_out to the section
 * size.  Returns NULL when the section is absent.  Valid while the cart
 * is open.
 */
const void *blyt_cart_find_section(const blyt_cart_t *cart, const char *name, size_t *size_out);

/* --- In-memory library registry ----------------------------------------- */

/*
 * Register a guest-side runtime library by name so dynlink can load it from
 * memory instead of from BLYT_LIB_DIR.  Useful for frontends (e.g. libretro)
 * that embed the guest libraries as binary data in the core itself.
 *
 * The data pointer must remain valid for the lifetime of any session that
 * uses it.  name must match the DT_NEEDED entry in the cart ELF exactly
 * (e.g. "libblyt32.so").  Duplicate registrations are silently ignored.
 */
void blyt_register_lib(const char *name, const void *data, size_t size);

/* Remove all registered in-memory libraries. */
void blyt_clear_libs(void);

/* --- Cart execution ------------------------------------------------------ */

/*
 * Called by the runtime to deliver a blyt_console_debug message to the
 * frontend. The string is NUL-terminated and lives only for the duration of
 * the callback.
 */
typedef void (*blyt_log_fn)(const char *msg);

/*
 * Called by the runtime at the end of each update+draw frame.
 * The frontend uses this to poll events, cap frame rate, present graphics,
 * etc.  May be NULL (headless / no per-frame host work needed).
 */
typedef void (*blyt_frame_fn)(void *userdata);

typedef enum blyt_cart_run_err {
    BLYT_RUN_OK = 0, /* cart exited cleanly */
    BLYT_RUN_ERR_EMU = 1, /* emulator setup failed */
    BLYT_RUN_ERR_ECALL_TRAP = 2, /* cart issued a non-permitted ecall */
    BLYT_RUN_ERR_ABORT = 3, /* cart called abort() */
    BLYT_RUN_FRAME_DONE = 4, /* one frame complete; call run_frame again */
    BLYT_RUN_GDB_PAUSED = 5, /* WASM: CPU is paused at a GDB breakpoint; poll GDB */
    BLYT_RUN_RESTART = 6, /* DAP client sent restart; call blyt_session_dap_reattach then wait */
    BLYT_RUN_FN_DONE = 7, /* host→guest fn call completed; call blyt_session_fn_return_value */
    BLYT_RUN_FN_ERROR = 8, /* ADR-0130: bridged Lua→native call raised a Lua error;
                            * the error value is on top of the exchange thread */
} blyt_cart_run_err_t;

/*
 * Execute the cart in the rv32emu emulator (blocking).
 * log_fn   — receives blyt_console_debug messages; NULL to discard.
 * frame_fn — called once per update+draw frame; NULL for headless execution.
 * userdata — passed through to frame_fn unchanged.
 */
blyt_cart_run_err_t blyt_cart_run(blyt_cart_t *cart, blyt_log_fn log_fn, blyt_frame_fn frame_fn,
                                  void *userdata);

/* --- Session API (frame-by-frame execution for libretro / WASM) ---------- */

/*
 * A session holds rv32emu state for one cart run, allowing the caller to
 * drive execution one frame at a time rather than blocking until exit.
 *
 * Lifecycle:
 *   s = blyt_session_create(cart, log_fn)
 *   while ((err = blyt_session_run_frame(s)) == BLYT_RUN_FRAME_DONE) { ... }
 *   blyt_session_destroy(s)
 */
typedef struct blyt_session blyt_session_t;
typedef struct blyt_state_ctx blyt_state_ctx_t;

/* Create a session for the given cart.  Returns NULL on failure.
 * The cart must outlive the session. */
blyt_session_t *blyt_session_create(blyt_cart_t *cart, blyt_log_fn log_fn);

/* Create a session in ADR-0130 host-Lua bridge mode (#232): the cart's native
 * half runs under rv32emu but links the bridge stub (libblyt32lua-bridge.so)
 * instead of the in-guest Lua VM, so its Lua C API calls trap to a host-side Lua
 * VM.  Used by the host-Lua fast path (WASM run_lua_cart, native
 * cart_run_hostlua) for hybrid carts; pair with blyt_session_lua_bridge_attach.
 * Identical to blyt_session_create otherwise.  Returns NULL on failure. */
blyt_session_t *blyt_session_create_lua_bridge(blyt_cart_t *cart, blyt_log_fn log_fn);

/*
 * Run the cart until the next BLYT_ECALL_FRAME_DONE boundary or until it
 * halts.  Returns BLYT_RUN_FRAME_DONE when the frame completed normally.
 * Returns BLYT_RUN_OK or an error code when the cart exits or faults.
 */
blyt_cart_run_err_t blyt_session_run_frame(blyt_session_t *session);

/* Destroy a session and free all emulator resources. */
void blyt_session_destroy(blyt_session_t *session);

/* Reload the session's resource table from its source (issue #91): the cart's
 * embedded sections, or — for a dev project-dir build — the staging directory
 * alongside the dev ELF.  Used by the `update_assets` dev-control command to
 * hot-swap edited assets between frames without a VM restart.  Returns true on
 * success.  `cart` must be the cart the session was created from. */
bool blyt_session_reload_resources(blyt_session_t *session, blyt_cart_t *cart);

/* Swap the running cart's code in place WITHOUT recreating the VM/session
 * (issue #127, spike-W β / gate G1 — the foundation for native hot reload).
 *
 * Unloads the current cart image and loads `new_cart` at guest base `load_base`,
 * re-links it against the persistent runtime libs (libblyt32/libblyt32lua/…),
 * re-resolves the cart's lifecycle entry points, and re-boots the cart so the
 * next blyt_session_run_frame() runs the new code's init().  The rv32 VM,
 * libblyt32 state, GDB state, palette, save dir and frame counter all PERSIST —
 * blyt_session_vm_id() is unchanged across the swap, proving the VM was reused
 * rather than recreated (which would cross the rv32emu single-VM-per-process
 * global-state hazard, issue #44).
 *
 * `load_base` is the guest base for the new image; pass 0 for the cart's native
 * bias (as today).  `reported_path`, when non-NULL, overrides the path recorded
 * for the GDB layout (NULL = new_cart->path); callers driving a run-mode reload
 * pass NULL.  load_base + reported_path are parameterised so the debug layer
 * (issue #119) can re-map at a fresh checksum path per reload without touching
 * the loader; a run-mode reload reloads at the same base with neither.
 *
 * State restore is the CALLER's job: snapshot via blyt_session_snapshot() before
 * the swap, then blyt_session_restore(reason=BLYT_LOAD_HOT_RELOAD) after the
 * post-swap boot frame — exactly as a fresh-load reload does, but in place.
 *
 * Returns true on success; on failure the session is left running the old code.
 */
bool blyt_session_swap_cart(blyt_session_t *session, const blyt_cart_t *new_cart,
                            uint32_t load_base, const char *reported_path);

/* Guest load base for the next debug hot reload (issue #119): a fresh slot the
 * cart is not currently mapped at, so the swap relocates it (and lldb, seeing a
 * new base + unique path, re-reads the rebuilt DWARF).  Pass to
 * blyt_session_swap_cart as load_base on a reload-while-debugging. */
uint32_t blyt_session_next_reload_base(const blyt_session_t *session);

/* Announce a just-swapped cart to the attached debugger (issue #119) so lldb
 * re-reads the rebuilt cart's DWARF and rebinds breakpoints to the new
 * addresses — a single clean location, no stale module.  Call AFTER
 * blyt_session_swap_cart with the SAME new_cart/load_base/reported_path.
 *
 * Performs a two-phase shared-library swap that mirrors a dlopen-then-dlclose
 * rendezvous: (1) announce the rebuilt cart as a NEW library (fresh base +
 * unique path) so lldb loads it and re-resolves breakpoints onto the new code,
 * then (2) drop the previous cart library so its stale location is removed —
 * leaving exactly one location per breakpoint.  A single combined event makes
 * lldb unload the old module without loading the new one, so the two phases are
 * sequenced via the client's library-list reads.  No-op when GDB is inactive. */
void blyt_session_gdb_notify_cart_reloaded(blyt_session_t *session, const blyt_cart_t *new_cart,
                                           uint32_t load_base, const char *reported_path);

/* True when this session is being actively debugged over GDB (issue #170): GDB
 * is enabled (a listener was started via blyt_session_gdb_listen) and a cart
 * library is registered.  The WASM frontend uses this to choose the debug-aware
 * reload sequence (fresh base + async solib re-arm) over the run-mode reload.
 * (Note: the WASM relay never marks a client "connected" the way TCP does, so
 * this gates on GDB being enabled, not on fc_gdb_stub_has_client.) */
bool blyt_session_gdb_is_debugging(const blyt_session_t *session);

/* NON-BLOCKING two-phase solib swap for the WASM debug hot reload (issue #170).
 *
 * blyt_session_gdb_notify_cart_reloaded performs the two-phase add-then-remove
 * library swap with a SYNCHRONOUS busy-wait (publish_libs_and_wait) between
 * phases — correct for the native TCP transport whose background thread services
 * the client concurrently, but unusable on WASM where the single-threaded gdb
 * stub is polled per animation tick and the relay only delivers packets when the
 * event loop runs.  These two entry points drive the same swap across async
 * main-loop ticks instead:
 *
 *   _begin publishes phase 1 (announce the rebuilt cart as a NEW library at its
 *   fresh base/path, firing the solib-change stop) and returns immediately.  The
 *   caller must NOT run cart frames until the swap finishes — it pumps _pump()
 *   each tick (which polls the stub so the client's library reads + continue are
 *   processed); once the client has consumed phase 1, _pump publishes phase 2
 *   (drop the stale entry) and, once that too is consumed, returns true.
 *
 * _begin returns false when GDB is not actively debugging (caller then takes the
 * run-mode path); _pump returns true (done) when no swap is in progress.  _pump
 * also force-completes after a bounded number of ticks so a debug session with
 * no live client cannot wedge the reload. */
bool blyt_session_gdb_reload_notify_begin(blyt_session_t *session, const blyt_cart_t *new_cart,
                                          uint32_t load_base, const char *reported_path);
bool blyt_session_gdb_reload_notify_pump(blyt_session_t *session);

/* Opaque identity of the underlying rv32 VM.  Stable for the life of a session
 * and across blyt_session_swap_cart (which reuses the VM); a freshly created
 * session has a different id.  For tests/introspection only. */
const void *blyt_session_vm_id(const blyt_session_t *session);

/* Notify the cart that hot-swapped assets changed (issue #122), after a
 * blyt_session_reload_resources() on the dev-mode `update_assets` path.  Invokes
 * the cart's optional blyt_cart_on_assets_reloaded(ids, n) — or, for a Lua cart,
 * the global on_assets_reloaded(ids) — with the changed resource ids, so a cart
 * that derived something from a resource (parsed/cached at load) can re-derive
 * only the affected ones instead of having to re-read every frame.  Dev-only:
 * only ever reached via update_assets (no VM restart); a code reload re-runs
 * init and never fires this.  No-op when the cart defines no callback or n==0.
 * Preserves emulator PC/registers, like the other host-initiated hooks. */
void blyt_session_notify_assets_reloaded(blyt_session_t *session, const uint32_t *ids, size_t n);

/* --- Frame output -------------------------------------------------------- */

/* Dimensions of the framebuffer. */
#define BLYT_FRAME_W 320
#define BLYT_FRAME_H 240

/*
 * The runtime keeps an internal palette-indexed framebuffer.  Before the cart
 * issues any drawing call the runtime fills it with the PM5544 test card.
 * After the first drawing call it reflects the cart's rendered output.
 *
 * Frontends expand to their preferred pixel format by doing a palette lookup:
 *   blyt_session_expand_frame(session, xrgb_buf);   // convenience: XRGB8888
 * or by reading the raw buffers and doing their own conversion:
 *   blyt_session_get_pixels(session)   → uint8_t[320*240] palette indices
 *   blyt_session_get_palette(session)  → uint32_t[256]   XRGB8888 entries
 * Both pointers are valid for the lifetime of the session.
 */
const uint8_t *blyt_session_get_pixels(const blyt_session_t *session);
const uint32_t *blyt_session_get_palette(const blyt_session_t *session);

/* True once the cart has issued any drawing call this run (the runtime stops
 * compositing the test card after the first one).  The WASM host-Lua fast path
 * reads this to decide test-card-vs-frame for a hybrid cart whose native half
 * drew into the session framebuffer while its host-Lua half did not (#193). */
bool blyt_session_cart_has_drawn(const blyt_session_t *session);

/* Set the cart lifecycle phase (blyt_phase.h: NONE/INIT/UPDATE/DRAW) on the
 * session's run context.  On the emulated path the guest blyt_main sets this via
 * BLYT_ECALL_PHASE; the WASM host-Lua fast path drives update/draw itself (no
 * blyt_main), so it calls this to mirror the phase — keeping surface access
 * draw()-only for a hybrid cart's native half, which still reaches the phase
 * gate through the gfx ECALL handlers (#205). */
void blyt_session_set_phase(blyt_session_t *session, int32_t phase);

/* --- Session surface registry, host-side access (WASM host-Lua fast path) ---
 *
 * A WASM hybrid cart runs its Lua half on the host-Lua fast path and its native
 * half in this rv32 session.  To keep surface handles (and the screen lock)
 * coherent with every other leg — where a hybrid runs in ONE registry — the
 * fast path's Lua surface bindings delegate to the session's surface registry
 * through these host entry points instead of keeping a separate pool (#210).
 * Because the host-Lua half shares the host address space, materialization is a
 * direct pointer into the canonical buffer (no ECALL, no copy).  These mirror
 * the tier-1 BLYT_ECALL_SURFACE_* handlers; they are a no-op / return
 * BLYT_HANDLE_NONE when session is NULL.
 *
 * The frame-loop reap is driven per real frame (blyt_session_reap_surfaces),
 * NOT at every blyt_session_run_frame() entry, because the fast-path trampoline
 * calls run_frame once per C-export call — reaping there would destroy a
 * Lua-created surface mid-frame. */

/* Create a blank off-screen surface (charged against the unified budget, #158).
 * Returns a SURFACE handle or BLYT_HANDLE_NONE. */
uint32_t blyt_session_surface_create(blyt_session_t *session, int32_t w, int32_t h);

/* Destroy an off-screen surface by handle.  No-op on the screen, a stale/foreign
 * handle, or a locked surface (#207 exclusive-lock reject). */
void blyt_session_surface_destroy(blyt_session_t *session, uint32_t handle);

/* Resolve a surface handle to its canonical drawable buffer for a tier-1 op.
 * Returns the buffer pointer and writes the dims + screen flag to the outs, or NULL
 * when the handle is unresolvable (wrong kind / stale generation) or the surface
 * is held by a tier-2 lock (#207 reject).  The pointer is the canonical buffer,
 * so writes are immediately coherent with the native half (same registry, same
 * address space). */
uint8_t *blyt_session_surface_drawable(blyt_session_t *session, uint32_t handle, int32_t *out_w,
                                       int32_t *out_h, bool *out_is_screen);

/* Acquire a tier-2 per-pixel lock on a surface for a host-Lua caller (#208).
 * Marks the slot locked (so tier-1 ops on it are rejected — #207 within-registry,
 * #210 cross-half for a hybrid's shared screen) and returns a DIRECT pointer to
 * the canonical buffer (no guest-VA copy); writes the dims + a release token to
 * the outs.  Returns NULL (token BLYT_HANDLE_NONE) when the handle is
 * unresolvable or already locked.  Pair with blyt_session_surface_release. */
uint8_t *blyt_session_surface_acquire(blyt_session_t *session, uint32_t handle, int32_t *out_w,
                                      int32_t *out_h, uint32_t *out_token);

/* Release a host-Lua tier-2 lock by its token: clears the lock and bumps the
 * lock generation (so the token goes stale).  A no-op on a stale/foreign token.
 * No copy-out — a direct lock's writes already landed in the canonical buffer. */
void blyt_session_surface_release(blyt_session_t *session, uint32_t token);

/* Reap the session's draw-scoped off-screen surfaces (frees their buffers, bumps
 * generations, force-releases any leftover lock).  Called once per real frame by
 * the fast-path Lua frame loop; see the note above. */
void blyt_session_reap_surfaces(blyt_session_t *session);

/* Expand the current frame to XRGB8888 via palette lookup.
 * xrgb_out must hold at least BLYT_FRAME_W * BLYT_FRAME_H uint32_t. */
void blyt_session_expand_frame(const blyt_session_t *session, uint32_t *xrgb_out);

/* --- DAP debugging (optional, requires BLYT_DAP compile flag) ------------ */

/*
 * Start a DAP server alongside this session.  port=0 lets the OS pick a free
 * port; the actual port is written to *port_out.  Call before the first
 * blyt_session_run_frame().  Returns the actual port (>0) on success, -1 on
 * failure or when BLYT_DAP is not compiled in.
 */
int blyt_session_dap_listen(blyt_session_t *s, int *port_out);

/* Stop the DAP server and free its resources. Idempotent. */
void blyt_session_dap_shutdown(blyt_session_t *s);

/* Re-enable DAP on a session that was recreated after a restart (e.g. after
 * retro_reset()).  The DAP server keeps running; this just marks the new
 * session as DAP-enabled so fc_dap_wait_configuration_done() will be reached. */
void blyt_session_dap_reattach(blyt_session_t *s);

/*
 * Block until the connected DAP client sends configurationDone (meaning all
 * breakpoints are registered) or the server shuts down.  Call this after
 * blyt_session_dap_listen() and before the first blyt_session_run_frame() to
 * ensure breakpoints are in place when the cart starts executing.
 * Returns non-zero if configurationDone was received; 0 if shutting down.
 * No-op (returns 0) when BLYT_DAP is not compiled in or DAP is not active.
 */
int blyt_session_dap_wait_ready(blyt_session_t *s);

/* --- GDB debugging (optional, requires BLYT_GDB compile flag) -------------- */

/*
 * Start a GDB RSP listener alongside this session.
 * port=0 lets the OS pick a free port (TCP) or is used as the relay port (WASM).
 * The actual port is written to *port_out.
 * Returns the actual port (>0) on success, -1 on failure or when BLYT_GDB is
 * not compiled in.
 */
int blyt_session_gdb_listen(blyt_session_t *s, int *port_out);

/* Override the filesystem path reported for the cart in the GDB
 * qXfer:libraries-svr4 list (issue #144).  lldb-dap opens this path locally to
 * read the cart's ELF sections + DWARF; a native session already reports the
 * real on-disk cart path, but the WASM runtime loads the cart from an in-memory
 * virtual path ("/cart.blyt") that the host-side lldb cannot open — so cart
 * breakpoints never bind and the session hangs.  The WASM frontend calls this
 * with a host-resolvable path to the same debug cart ELF.  No-op if GDB is not
 * enabled, no cart library is registered, or host_path is empty. */
void blyt_session_gdb_set_cart_path(blyt_session_t *s, const char *host_path);

/* Stop the GDB server. Idempotent. */
void blyt_session_gdb_shutdown(blyt_session_t *s);

/* --- Host→guest function calls (WASM hybrid Lua+C carts) ------------------- */

/*
 * Begin a host→guest function call.  Sets rv PC=fn_addr, RA=FN_RETURN trampoline,
 * and a0..a3 to args[0..nargs-1] (nargs capped at 4).
 * Drive with blyt_session_run_frame() until it returns BLYT_RUN_FN_DONE.
 */
int blyt_session_begin_fn_call(blyt_session_t *s, uint32_t fn_addr, int nargs,
                               const uint32_t args[]);

/* After BLYT_RUN_FN_DONE: read the function's return value from rv register a0. */
uint32_t blyt_session_fn_return_value(const blyt_session_t *s);

/*
 * Check whether cart-native code called blyt_quit() during the last
 * trampoline invocation.  Internally calls blyt_is_quit_requested() in the
 * RV32 guest and returns 1 if quit was requested, 0 otherwise.
 * The WASM frontend calls this after each BLYT_RUN_FN_DONE to propagate the
 * guest quit signal to the Lua coroutine's blyt.should_quit() check.
 * Returns 0 when blyt_is_quit_requested is absent from the session's symtab.
 */
int blyt_session_check_guest_quit(blyt_session_t *s);

/*
 * Cart-native lifecycle callback addresses for WASM hybrid dispatch.
 * Each function returns the resolved guest address of the named lifecycle symbol
 * if it is defined in the cart's own code (bias=0, addr < GUEST_LIB_BASE), or 0
 * if the symbol resolves to a runtime library stub.  The WASM frontend uses
 * these to inject Lua trampolines for cart-native lifecycle overrides.
 */
uint32_t blyt_session_cart_fn_init(blyt_session_t *s);
uint32_t blyt_session_cart_fn_on_new_state(blyt_session_t *s);
uint32_t blyt_session_cart_fn_update(blyt_session_t *s);
uint32_t blyt_session_cart_fn_draw(blyt_session_t *s);
uint32_t blyt_session_cart_fn_on_quit(blyt_session_t *s);
uint32_t blyt_session_cart_fn_cleanup(blyt_session_t *s);

/* Accessors for WASM state API wiring. */
blyt_state_ctx_t *blyt_session_state_ctx(blyt_session_t *s);
const char *blyt_session_save_dir(blyt_session_t *s);
const char *blyt_session_cart_name(blyt_session_t *s);

/* --- ECALL-bridged Lua C API (ADR-0130, WASM hybrid carts) ----------------- */

/* .lua_exports entry flags (byte after ret_type; 0 in pre-ADR-0130 carts). */
#define BLYT_LUA_EXPORT_FLAG_BRIDGED 0x01u

struct lua_State; /* opaque here; only the WASM frontend passes a real one */

/*
 * Attach the exchange thread for bridged Lua→native calls.  The frontend
 * creates it (lua_newthread, registry-anchored) and the bridge executes all
 * BLYT_ECALL_LUA_OP operations against its stack.  No-op without BLYT_LUA.
 */
void blyt_session_lua_bridge_attach(blyt_session_t *s, struct lua_State *exch);

/*
 * Begin a bridged Lua→native call: the wrapper at wrap_addr is invoked with
 * an opaque call token as its lua_State*; its Lua arguments must already be
 * on the exchange thread (lua_xmove'd from the calling coroutine).  Drive
 * with blyt_session_run_frame() until BLYT_RUN_FN_DONE (a0 = number of
 * return values left on the exchange thread, read via
 * blyt_session_fn_return_value) or BLYT_RUN_FN_ERROR (error value on top of
 * the exchange thread; guest registers restored).
 */
int blyt_session_begin_bridged_call(blyt_session_t *s, uint32_t wrap_addr);

/*
 * Visitor callback for blyt_session_visit_lua_exports.
 * Called once per exported function with its Lua name, guest function address,
 * wrapper address (nonzero only for bridged exports), flags
 * (BLYT_LUA_EXPORT_FLAG_*), argument count, argument types (BLYT_LUA_TYPE_*
 * constants), and return type.
 */
typedef void (*blyt_lua_export_visitor_t)(const char *lua_name, uint32_t fn_guest_addr,
                                          uint32_t wrap_guest_addr, uint8_t flags, uint8_t nargs,
                                          const uint8_t arg_types[4], uint8_t ret_type,
                                          void *userdata);

/*
 * Iterate all Lua exports parsed from the cart's .lua_exports section.
 * Calls cb once per export.  No-op when BLYT_LUA is not compiled in or
 * the cart has no .lua_exports section.
 */
void blyt_session_visit_lua_exports(blyt_session_t *s, blyt_lua_export_visitor_t cb,
                                    void *userdata);

/*
 * Block until the GDB client attaches and sends vCont (ready to run).
 * Optional — callers may skip if they don't want to gate on client attachment.
 * No-op when BLYT_GDB is not compiled in.
 */
int blyt_session_gdb_wait_attached(blyt_session_t *s);

/*
 * In hybrid (DAP+GDB) mode, clear the initial GDB halt so the cart runs
 * without waiting for a vCont;c from the GDB client.  Call this after both
 * blyt_session_dap_wait_ready() and blyt_session_gdb_wait_attached() return,
 * so Lua breakpoints are already registered before the cart executes.
 * No-op when BLYT_GDB is not compiled in, or if the stub is not in the
 * initial-halt state.
 */
void blyt_session_gdb_continue_initial_halt(blyt_session_t *s);

/*
 * Block until the GDB client has finished its initial configuration (fetched the
 * library list, inserted its breakpoints, and issued its first vCont;c) so a
 * native breakpoint's ebreak is patched in before any cart code runs (issue
 * #119).  Used by the hybrid gate in place of immediately force-clearing the
 * initial halt: an early native call would otherwise cache a translated block
 * with no ebreak that the later insertion never re-translates (rv32emu single-VM
 * block cache, cf. #42).  Returns 1 once the client has continued, 0 on timeout
 * (the caller should then force-clear the halt so boot cannot wedge).  No-op /
 * returns 0 when BLYT_GDB is not compiled in or on WASM (async transport).
 */
int blyt_session_gdb_wait_client_continue(blyt_session_t *s);

/* --- --reset-every-frame cycle (save-state stress testing) ---------------- */

/*
 * Execute one reset-every-frame cycle against an active session:
 *   1. Call blyt_cart_on_save_state() in the guest.
 *   2. Snapshot all state buffer contents.
 *   3. Zero state buffers and guest BSS (static vars reset to 0).
 *   4. Call blyt_cart_init().
 *   5. Restore state buffer snapshot.
 *   6. Call blyt_cart_on_load_state(HOT_RELOAD) in the guest.
 * The emulator's PC and registers are saved before the cycle and restored
 * afterwards, so the normal game loop continues from the same point.
 */
void blyt_reset_every_frame_cycle(blyt_session_t *s);

/* Force-evict every eviction-eligible resource in the session's table (ADR-0027
 * v2, #137): the "evict all evictable now" forcing primitive behind the
 * per-leg --evict-every-frame / BLYT_RESOURCE_EVICT_EVERY_FRAME test hook. Frees
 * the owned/decompressed bytes of entries with no load/pin reference; the next
 * access rehydrates byte-identically. Returns the total bytes reclaimed. This is
 * a deterministic test trigger; real pressure-driven eviction is wired in #158. */
size_t blyt_session_resource_evict_all(blyt_session_t *s);

/* --- Dev control channel host operations (issue #87) ----------------------
 *
 * Drive runtime lifecycle from a frontend's dev control server without
 * disturbing the running game loop: each call saves and restores emulator
 * state around the guest calls it makes, so the next blyt_session_run_frame()
 * continues from the same point.
 */

/* Opaque migratable snapshot of all state buffers (defined in state_buffer.h).
 * C11 permits this redundant typedef alongside that header. */
typedef struct blyt_state_snapshot blyt_state_snapshot_t;

/*
 * Persist state buffers to / restore them from disk save slot `slot`, driving
 * the cart's on_save_state / on_load_state hooks.  Return 0 on success.
 */
int blyt_session_save_state(blyt_session_t *s, uint32_t slot);
int blyt_session_load_state(blyt_session_t *s, uint32_t slot);

/*
 * Capture a snapshot of all state buffers after flushing transient state via
 * on_save_state.  Returns NULL if the session has no state or on failure; the
 * caller owns the result.  Used for hot reload, where the session is recreated
 * from fresh code between snapshot and restore.
 */
blyt_state_snapshot_t *blyt_session_snapshot(blyt_session_t *s);

/* Free a snapshot returned by blyt_session_snapshot without restoring it
 * (e.g. when a reload aborts before the new session is ready). */
void blyt_session_snapshot_free(blyt_state_snapshot_t *snap);

/*
 * Restore a snapshot into the session's state buffers and notify the cart via
 * on_load_state(reason).  Takes ownership of `snap` (frees it).  `reason` uses
 * the guest BLYT_LOAD_* values (3 = HOT_RELOAD).
 */
void blyt_session_restore(blyt_session_t *s, blyt_state_snapshot_t *snap, uint32_t reason);

#ifdef __cplusplus
}
#endif
