/* frontends/libretro/runner.c
 *
 * blyt-libretro-runner — minimal libretro frontend for debug test orchestration.
 *
 * Loads blyt_libretro.so via dlopen, drives retro_run() until the cart exits,
 * and echoes port-announcement log messages to stdout so that test orchestrators
 * (run_libretro_dap_test.mjs, run_libretro_gdb_test.mjs) can parse them with
 * the same regex used for blytplay.
 *
 * Usage:
 *   blyt-libretro-runner <blyt_libretro.so> <cart.blyt>
 *
 * Environment (read by blyt_libretro.c):
 *   BLYT_DAP_PORT=0   — enable DAP server on an OS-assigned port
 *   BLYT_GDB_PORT=0   — enable GDB server on an OS-assigned port
 *
 * Exit 0 on clean blyt_exit, 1 on timeout or load error.
 */

#include <dlfcn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libretro.h"

#define MAX_FRAMES 60000 /* 1000 s at 60 fps */

/* ── Log callback ───────────────────────────────────────────────────────── */

static void RETRO_CALLCONV runner_log(enum retro_log_level level,
                                      const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);

    /* Non-errors go to stdout so orchestrators can parse port announcements.
     * Errors go to stderr. */
    FILE *out = (level == RETRO_LOG_ERROR) ? stderr : stdout;
    fputs(buf, out);
    fflush(out);
}

/* ── Stub callbacks ─────────────────────────────────────────────────────── */

static bool env_cb(unsigned cmd, void *data) {
    if (cmd == RETRO_ENVIRONMENT_GET_LOG_INTERFACE) {
        struct retro_log_callback *cb = (struct retro_log_callback *)data;
        cb->log = runner_log;
        return true;
    }
    if (cmd == RETRO_ENVIRONMENT_SET_PIXEL_FORMAT)
        return true;
    return false;
}

static void video_refresh(const void *d, unsigned w, unsigned h, size_t p) {
    (void)d; (void)w; (void)h; (void)p;
}
static void audio_sample(int16_t l, int16_t r) { (void)l; (void)r; }
static size_t audio_batch(const int16_t *d, size_t f) { (void)d; return f; }
static void input_poll(void) {}
static int16_t input_state(unsigned p, unsigned d, unsigned i, unsigned id) {
    (void)p; (void)d; (void)i; (void)id; return 0;
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr,
                "usage: blyt-libretro-runner <blyt_libretro.so> <cart.blyt>\n"
                "\n"
                "Environment:\n"
                "  BLYT_DAP_PORT=0  enable DAP debug server\n"
                "  BLYT_GDB_PORT=0  enable GDB debug server\n");
        return 1;
    }

    void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        fprintf(stderr, "dlopen(%s): %s\n", argv[1], dlerror());
        return 1;
    }

#define LOAD(name)                                                            \
    __typeof__(name) *p_##name = (__typeof__(name) *)dlsym(lib, #name);       \
    if (!p_##name) {                                                          \
        fprintf(stderr, "dlsym(%s): %s\n", #name, dlerror());                 \
        dlclose(lib);                                                         \
        return 1;                                                             \
    }

    LOAD(retro_set_environment)
    LOAD(retro_set_video_refresh)
    LOAD(retro_set_audio_sample)
    LOAD(retro_set_audio_sample_batch)
    LOAD(retro_set_input_poll)
    LOAD(retro_set_input_state)
    LOAD(retro_init)
    LOAD(retro_deinit)
    LOAD(retro_load_game)
    LOAD(retro_run)
    LOAD(retro_unload_game)
#undef LOAD

    /* blyt-specific non-standard exports. */
    bool (*p_is_done)(void) =
        (bool (*)(void))dlsym(lib, "blyt_libretro_is_done");
    bool (*p_dap_wait)(void) =
        (bool (*)(void))dlsym(lib, "blyt_libretro_dap_wait_ready");
    bool (*p_gdb_wait)(void) =
        (bool (*)(void))dlsym(lib, "blyt_libretro_gdb_wait_attached");

    if (!p_is_done) {
        fprintf(stderr, "dlsym(blyt_libretro_is_done): %s\n", dlerror());
        dlclose(lib);
        return 1;
    }

    p_retro_set_environment(env_cb);
    p_retro_set_video_refresh(video_refresh);
    p_retro_set_audio_sample(audio_sample);
    p_retro_set_audio_sample_batch(audio_batch);
    p_retro_set_input_poll(input_poll);
    p_retro_set_input_state(input_state);
    p_retro_init();

    struct retro_game_info game = {argv[2], NULL, 0, NULL};
    if (!p_retro_load_game(&game)) {
        fprintf(stderr, "retro_load_game failed\n");
        p_retro_deinit();
        dlclose(lib);
        return 1;
    }

    /* Wait for debug clients to connect and complete initialisation handshake.
     * Mirrors the blytplay sequence in frontends/player/main.c:224,230. */
    if (getenv("BLYT_DAP_PORT") && p_dap_wait)
        p_dap_wait();
    if (getenv("BLYT_GDB_PORT") && p_gdb_wait)
        p_gdb_wait();

    int rc = 1;
    for (int frame = 0; frame < MAX_FRAMES; frame++) {
        p_retro_run();
        if (p_is_done()) {
            rc = 0;
            break;
        }
    }

    if (rc != 0)
        fprintf(stderr, "cart did not exit after %d frames\n", MAX_FRAMES);

    p_retro_unload_game();
    p_retro_deinit();
    dlclose(lib);
    return rc;
}
