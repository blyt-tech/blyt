#pragma once

/* blyt_runtime.h — host/frontend-facing API (not shipped in the cart SDK). */

#ifdef __cplusplus
extern "C" {
#endif

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

/*
 * Run the cart until the next BLYT_ECALL_FRAME_DONE boundary or until it
 * halts.  Returns BLYT_RUN_FRAME_DONE when the frame completed normally.
 * Returns BLYT_RUN_OK or an error code when the cart exits or faults.
 */
blyt_cart_run_err_t blyt_session_run_frame(blyt_session_t *session);

/* Destroy a session and free all emulator resources. */
void blyt_session_destroy(blyt_session_t *session);

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
