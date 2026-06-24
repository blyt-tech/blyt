/*
 * test_session_api — exercises blyt_session_t and the in-memory lib registry.
 *
 * Usage:
 *   test_session_api session  <cart.blyt> <lib_dir>
 *   test_session_api registry <cart.blyt> <lib_dir>
 *   test_session_api swap     <v1.blyt> <v2.blyt> <lib_dir>
 *
 * session  mode: drives the cart via blyt_session_create/run_frame/destroy
 *               with libs loaded from BLYT_LIB_DIR (set to lib_dir).
 * registry mode: reads the .so files from lib_dir, registers them with
 *               blyt_register_lib, then creates a session without setting
 *               BLYT_LIB_DIR — exercising the in-memory registry path.
 * swap     mode: creates a session on v1, runs a few frames, then swaps the
 *               cart code to v2 IN PLACE via blyt_session_swap_cart (issue
 *               #127, spike-W β / G1) without recreating the VM, and asserts
 *               the VM instance persisted (blyt_session_vm_id unchanged) and
 *               the v2 code booted and ran a clean frame.  Prints each cart's
 *               init marker so the caller can confirm v2's code went live.
 *
 * All modes print the blyt_console_debug output to stdout and exit 0 on
 * success.  Exit 1 on any failure.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blyt_runtime.h"

static int g_failures = 0;

static void log_fn(const char *msg) {
    printf("%s\n", msg);
}

/* -------------------------------------------------------------------------
 * Read an entire file into a heap buffer.  Caller frees.
 * ------------------------------------------------------------------------- */

static void *read_file(const char *path, size_t *size_out) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "test_session_api: cannot open %s\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(f);
        return NULL;
    }
    void *buf = malloc((size_t)sz);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size_out = (size_t)sz;
    return buf;
}

/* -------------------------------------------------------------------------
 * Run the cart through the session API.  Returns 0 on success.
 * ------------------------------------------------------------------------- */

static int run_via_session(const char *cart_path) {
    blyt_cart_t *cart = NULL;
    blyt_cart_err_t cerr = blyt_cart_open(cart_path, &cart);
    if (cerr != BLYT_CART_OK) {
        fprintf(stderr, "blyt_cart_open: %s\n", blyt_cart_err_str(cerr));
        return 1;
    }

    blyt_session_t *s = blyt_session_create(cart, log_fn);
    if (!s) {
        fprintf(stderr, "blyt_session_create returned NULL\n");
        blyt_cart_close(cart);
        return 1;
    }

    int frame_done_count = 0;
    blyt_cart_run_err_t err;
    while ((err = blyt_session_run_frame(s)) == BLYT_RUN_FRAME_DONE)
        frame_done_count++;

    blyt_session_destroy(s);
    blyt_cart_close(cart);

    if (err != BLYT_RUN_OK) {
        fprintf(stderr, "session exited with error %d\n", (int)err);
        return 1;
    }
    if (frame_done_count == 0) {
        fprintf(stderr, "cart exited without firing BLYT_ECALL_FRAME_DONE\n");
        return 1;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * mode: session — use BLYT_LIB_DIR (set by caller via environment)
 * ------------------------------------------------------------------------- */

static int mode_session(const char *cart_path, const char *lib_dir) {
    if (setenv("BLYT_LIB_DIR", lib_dir, 1) != 0) {
        fprintf(stderr, "setenv BLYT_LIB_DIR failed\n");
        return 1;
    }
    return run_via_session(cart_path);
}

/* -------------------------------------------------------------------------
 * mode: registry — load .so files into memory and register them, then run
 *                  without BLYT_LIB_DIR set.
 * ------------------------------------------------------------------------- */

static int mode_registry(const char *cart_path, const char *lib_dir) {
    char path[4096];
    size_t sz;
    void *bufs[3] = {NULL, NULL, NULL};
    static const char *names[] = {"libblytcommon.so", "libblytc.so", "libblyt32.so"};

    for (int i = 0; i < 3; i++) {
        snprintf(path, sizeof(path), "%s/%s", lib_dir, names[i]);
        bufs[i] = read_file(path, &sz);
        if (!bufs[i]) {
            fprintf(stderr, "failed to read %s\n", path);
            for (int j = 0; j < i; j++)
                free(bufs[j]);
            return 1;
        }
        /* Re-read size for the register call */
    }

    /* Re-read with sizes for registration */
    blyt_clear_libs();
    for (int i = 0; i < 3; i++) {
        free(bufs[i]);
        snprintf(path, sizeof(path), "%s/%s", lib_dir, names[i]);
        bufs[i] = read_file(path, &sz);
        blyt_register_lib(names[i], bufs[i], sz);
    }

    /* Clear BLYT_LIB_DIR so the registry must be used */
    unsetenv("BLYT_LIB_DIR");

    int result = run_via_session(cart_path);

    blyt_clear_libs();
    for (int i = 0; i < 3; i++)
        free(bufs[i]);

    return result;
}

/* -------------------------------------------------------------------------
 * mode: swap — create a session on v1, run a few frames, then swap the cart
 *              code to v2 IN PLACE (issue #127) without recreating the VM.
 * ------------------------------------------------------------------------- */

static int mode_swap(const char *v1_path, const char *v2_path, const char *lib_dir) {
    if (setenv("BLYT_LIB_DIR", lib_dir, 1) != 0) {
        fprintf(stderr, "setenv BLYT_LIB_DIR failed\n");
        return 1;
    }

    blyt_cart_t *c1 = NULL;
    if (blyt_cart_open(v1_path, &c1) != BLYT_CART_OK) {
        fprintf(stderr, "swap: open v1 failed\n");
        return 1;
    }
    blyt_session_t *s = blyt_session_create(c1, log_fn);
    if (!s) {
        fprintf(stderr, "swap: session create failed\n");
        blyt_cart_close(c1);
        return 1;
    }

    /* Run a few frames so v1's init/loop is live before the swap. */
    for (int i = 0; i < 3; i++) {
        blyt_cart_run_err_t e = blyt_session_run_frame(s);
        if (e != BLYT_RUN_FRAME_DONE) {
            fprintf(stderr, "swap: v1 frame %d returned %d\n", i, (int)e);
            blyt_session_destroy(s);
            blyt_cart_close(c1);
            return 1;
        }
    }

    const void *vm_before = blyt_session_vm_id(s);

    /* Snapshot live state before the swap (NULL when the cart has no state
     * buffers, as the minimal C swap carts do — restore is then a no-op). */
    blyt_state_snapshot_t *snap = blyt_session_snapshot(s);

    /* Swap to v2 IN PLACE — same base, no GDB path-reporting (run-mode). */
    blyt_cart_t *c2 = NULL;
    if (blyt_cart_open(v2_path, &c2) != BLYT_CART_OK) {
        fprintf(stderr, "swap: open v2 failed\n");
        blyt_session_snapshot_free(snap);
        blyt_session_destroy(s);
        blyt_cart_close(c1);
        return 1;
    }
    /* Optional non-zero load base (issue #119 debug reload re-maps the cart to a
     * fresh base each reload).  BLYT_SWAP_BASE overrides the default same-base. */
    uint32_t swap_base = 0u;
    const char *base_env = getenv("BLYT_SWAP_BASE");
    if (base_env && base_env[0])
        swap_base = (uint32_t)strtoul(base_env, NULL, 0);
    if (!blyt_session_swap_cart(s, c2, swap_base, NULL)) {
        fprintf(stderr, "swap: blyt_session_swap_cart returned false\n");
        blyt_session_snapshot_free(snap);
        blyt_session_destroy(s);
        blyt_cart_close(c1);
        blyt_cart_close(c2);
        return 1;
    }

    /* The VM instance must persist across the swap (not recreated). */
    if (blyt_session_vm_id(s) != vm_before) {
        fprintf(stderr, "swap: VM id changed across swap — session was recreated\n");
        blyt_session_snapshot_free(snap);
        blyt_session_destroy(s);
        blyt_cart_close(c1);
        blyt_cart_close(c2);
        return 1;
    }

    /* Boot the new cart: this frame runs v2's init() (which prints its marker). */
    blyt_cart_run_err_t e = blyt_session_run_frame(s);
    if (e != BLYT_RUN_FRAME_DONE) {
        fprintf(stderr, "swap: v2 boot frame returned %d\n", (int)e);
        blyt_session_snapshot_free(snap);
        blyt_session_destroy(s);
        blyt_cart_close(c1);
        blyt_cart_close(c2);
        return 1;
    }

    /* Restore state over the fresh v2 buffers and notify HOT_RELOAD (no-op when
     * snap is NULL); exercises the restore path doesn't fault post-swap. */
    blyt_session_restore(s, snap, 3u /* BLYT_LOAD_HOT_RELOAD */);

    /* v2 must remain runnable on the persisted VM. */
    e = blyt_session_run_frame(s);
    if (e != BLYT_RUN_FRAME_DONE && e != BLYT_RUN_OK) {
        fprintf(stderr, "swap: v2 post-restore frame returned %d\n", (int)e);
        blyt_session_destroy(s);
        blyt_cart_close(c1);
        blyt_cart_close(c2);
        return 1;
    }

    blyt_session_destroy(s);
    blyt_cart_close(c1);
    blyt_cart_close(c2);
    printf("swap-ok\n");
    return 0;
}

/* -------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */

int main(int argc, char *argv[]) {
    if (argc == 5 && strcmp(argv[1], "swap") == 0)
        return mode_swap(argv[2], argv[3], argv[4]);

    if (argc != 4) {
        fprintf(stderr, "usage: test_session_api session|registry <cart.blyt> <lib_dir>\n"
                        "       test_session_api swap <v1.blyt> <v2.blyt> <lib_dir>\n");
        return 1;
    }
    const char *mode = argv[1];
    const char *cart_path = argv[2];
    const char *lib_dir = argv[3];

    if (strcmp(mode, "session") == 0)
        return mode_session(cart_path, lib_dir);
    if (strcmp(mode, "registry") == 0)
        return mode_registry(cart_path, lib_dir);

    fprintf(stderr, "unknown mode: %s\n", mode);
    return 1;
}
