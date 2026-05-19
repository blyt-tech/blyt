/*
 * test_session_api — exercises blyt_session_t and the in-memory lib registry.
 *
 * Usage:
 *   test_session_api session  <cart.blyt> <lib_dir>
 *   test_session_api registry <cart.blyt> <lib_dir>
 *
 * session  mode: drives the cart via blyt_session_create/run_frame/destroy
 *               with libs loaded from BLYT_LIB_DIR (set to lib_dir).
 * registry mode: reads the .so files from lib_dir, registers them with
 *               blyt_register_lib, then creates a session without setting
 *               BLYT_LIB_DIR — exercising the in-memory registry path.
 *
 * Both modes print the blyt_console_debug output to stdout and exit 0 on
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
 * main
 * ------------------------------------------------------------------------- */

int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "usage: test_session_api session|registry <cart.blyt> <lib_dir>\n");
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
