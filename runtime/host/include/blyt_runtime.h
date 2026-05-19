#pragma once

/* blyt_runtime.h — host/frontend-facing API (not shipped in the cart SDK). */

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

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
    BLYT_CART_ERR_UNKNOWN_SECT = 10, /* unrecognised ELF section name */
    BLYT_CART_ERR_BAD_NEEDED = 11, /* DT_NEEDED allowlist violation */
    BLYT_CART_ERR_NO_CART_INFO = 12, /* .cart.info section missing */
    BLYT_CART_ERR_BAD_PREAMBLE = 13, /* section preamble tag or version mismatch */
    BLYT_CART_ERR_BAD_CART_INFO = 14, /* .cart.info FlatBuffers parse error */
    BLYT_CART_ERR_BAD_CART_CONFIG = 15, /* .cart.config FlatBuffers parse error */
    BLYT_CART_ERR_API_VERSION = 16, /* api_version unsupported */
    BLYT_CART_ERR_BAD_SEGMENT = 17, /* segment layout violation */
    BLYT_CART_ERR_BAD_INTERP = 18, /* PT_INTERP present (forbidden on custom-loader path) */
    BLYT_CART_ERR_NO_RELRO = 19, /* PT_GNU_RELRO absent */
    BLYT_CART_ERR_BAD_OPCODE = 20, /* ecall or ebreak found in executable segment */
    BLYT_CART_ERR_BAD_IMPORT = 21, /* imported symbol not on allowlist */
    BLYT_CART_ERR_MISSING_ENTRY = 22, /* required cart entry point absent */
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

/* Return a static human-readable string for a blyt_cart_err_t value. */
const char *blyt_cart_err_str(blyt_cart_err_t err);

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

#ifdef __cplusplus
}
#endif
