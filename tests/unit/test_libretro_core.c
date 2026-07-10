/* tests/unit/test_libretro_core.c
 *
 * Minimal libretro runner for e2e integration tests.
 *
 * Usage:
 *   test_libretro_core [--reset-every-frame] <blyt_libretro.so> <cart.blyt>
 *
 * Loads blyt_libretro.so via dlopen, wires up stub callbacks, and calls
 * retro_run in a loop until blyt_libretro_is_done() returns true or
 * MAX_FRAMES have elapsed.
 *
 * --reset-every-frame: after every retro_run, drive the core's
 * retro_reset_every_frame_cycle() — the same save-state stress cycle
 * blytplay's --reset-every-frame flag and the WASM runtime's
 * BLYT_RESET_EVERY_FRAME use (on_save_state → snapshot → zero BSS → init →
 * restore → on_load_state).  Carts that keep their state in tracked buffers
 * must behave identically under this cycle.
 *
 * Exit:
 *   0  — cart called blyt_quit() / blyt_exit(0) cleanly
 *   1  — error (load failure, ecall trap, abort, timeout)
 *
 * The log callback writes to stderr so test output is visible on failure.
 */

#include <dlfcn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libretro.h"

#define MAX_FRAMES 60000 /* 1000 s at 60 fps — generous timeout */

/* ── Libretro callbacks ─────────────────────────────────────────────────── */

static void RETRO_CALLCONV test_log(enum retro_log_level level, const char *fmt, ...) {
    (void)level;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static bool env_cb(unsigned cmd, void *data) {
    if (cmd == RETRO_ENVIRONMENT_GET_LOG_INTERFACE) {
        struct retro_log_callback *cb = (struct retro_log_callback *)data;
        cb->log = test_log;
        return true;
    }
    if (cmd == RETRO_ENVIRONMENT_SET_PIXEL_FORMAT)
        return true;
    return false;
}

static void video_refresh(const void *d, unsigned w, unsigned h, size_t p) {
    (void)d;
    (void)w;
    (void)h;
    (void)p;
}
static void audio_sample(int16_t l, int16_t r) {
    (void)l;
    (void)r;
}
static size_t audio_batch(const int16_t *d, size_t f) {
    (void)d;
    return f;
}
static void input_poll(void) {
}
static int16_t input_state(unsigned p, unsigned d, unsigned i, unsigned id) {
    (void)p;
    (void)d;
    (void)i;
    (void)id;
    return 0;
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    bool reset_every_frame = false;
    bool evict_every_frame = false; /* #137: force-evict all evictable each frame */
    /* Dev-mode asset hot-swap trigger (issue #122): at frame `update_assets_after`
     * point BLYT_RESOURCE_DIR at `resource_dir_v2` (if given) and drive the core's
     * blyt_libretro_update_assets() with `asset_ids`, simulating the update_assets
     * dev-control command a dev frontend would send.  -1 = no trigger. */
    long update_assets_after = -1;
    const char *resource_dir_v2 = NULL;
    uint32_t asset_ids[64];
    size_t asset_n = 0;
    /* Dev-mode cart-swap reload trigger (issue #124/#246): at frame `reload_after`
     * drive the core's blyt_libretro_reload_at(reload_path), simulating the
     * `reload` dev-control command — the whole cart (code + bundled resources) is
     * swapped, not just the resource index.  -1 = no trigger. */
    long reload_after = -1;
    const char *reload_path = NULL;
    /* Run exactly this many frames then exit 0, for hot-swap tests whose cart
     * intentionally never quits (it is observed via its per-frame output).
     * -1 = run until the cart quits / MAX_FRAMES (the default). */
    long run_frames = -1;
    int argi = 1;
    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] == '-') {
        if (strcmp(argv[argi], "--reset-every-frame") == 0) {
            reset_every_frame = true;
            argi++;
        } else if (strcmp(argv[argi], "--evict-every-frame") == 0) {
            evict_every_frame = true;
            argi++;
        } else if (strcmp(argv[argi], "--run-frames") == 0 && argi + 1 < argc) {
            run_frames = strtol(argv[argi + 1], NULL, 10);
            argi += 2;
        } else if (strcmp(argv[argi], "--update-assets-after") == 0 && argi + 1 < argc) {
            update_assets_after = strtol(argv[argi + 1], NULL, 10);
            argi += 2;
        } else if (strcmp(argv[argi], "--resource-dir-v2") == 0 && argi + 1 < argc) {
            resource_dir_v2 = argv[argi + 1];
            argi += 2;
        } else if (strcmp(argv[argi], "--reload-after") == 0 && argi + 1 < argc) {
            reload_after = strtol(argv[argi + 1], NULL, 10);
            argi += 2;
        } else if (strcmp(argv[argi], "--reload-path") == 0 && argi + 1 < argc) {
            reload_path = argv[argi + 1];
            argi += 2;
        } else if (strcmp(argv[argi], "--asset-ids") == 0 && argi + 1 < argc) {
            const char *s = argv[argi + 1];
            while (*s && asset_n < sizeof(asset_ids) / sizeof(asset_ids[0])) {
                char *e = NULL;
                long r = strtol(s, &e, 10);
                if (e == s)
                    break;
                asset_ids[asset_n++] = (uint32_t)r;
                s = (*e == ',') ? e + 1 : e;
            }
            argi += 2;
        } else {
            break;
        }
    }
    if (argc - argi < 2) {
        fprintf(stderr, "usage: test_libretro_core [--reset-every-frame] [--evict-every-frame] "
                        "[--update-assets-after N --resource-dir-v2 DIR --asset-ids a,b,c] "
                        "<blyt_libretro.so> <cart.blyt>\n");
        return 1;
    }
    const char *core_path = argv[argi];
    const char *cart_path = argv[argi + 1];

    void *lib = dlopen(core_path, RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        fprintf(stderr, "dlopen(%s): %s\n", core_path, dlerror());
        return 1;
    }

#define LOAD(name)                                                                                 \
    __typeof__(name) *p_##name = (__typeof__(name) *)dlsym(lib, #name);                            \
    if (!p_##name) {                                                                               \
        fprintf(stderr, "dlsym(%s): %s\n", #name, dlerror());                                      \
        dlclose(lib);                                                                              \
        return 1;                                                                                  \
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

    /* blyt_libretro_is_done: non-standard symbol exported by blyt_libretro.c
     * to signal that the cart called blyt_quit() / blyt_exit(). */
    bool (*p_is_done)(void) = (bool (*)(void))dlsym(lib, "blyt_libretro_is_done");
    if (!p_is_done) {
        fprintf(stderr, "dlsym(blyt_libretro_is_done): %s\n", dlerror());
        dlclose(lib);
        return 1;
    }

    /* retro_reset_every_frame_cycle: blyt-private save-state stress hook,
     * same cycle as blytplay --reset-every-frame (ADR-0045/0110). */
    void (*p_reset_cycle)(void) = NULL;
    if (reset_every_frame) {
        p_reset_cycle = (void (*)(void))dlsym(lib, "retro_reset_every_frame_cycle");
        if (!p_reset_cycle) {
            fprintf(stderr, "dlsym(retro_reset_every_frame_cycle): %s\n", dlerror());
            dlclose(lib);
            return 1;
        }
    }

    /* retro_resource_evict_all: blyt-private force-evict hook, same role as
     * blytplay --evict-every-frame (ADR-0027 v2, #137). */
    void (*p_evict_all)(void) = NULL;
    if (evict_every_frame) {
        p_evict_all = (void (*)(void))dlsym(lib, "retro_resource_evict_all");
        if (!p_evict_all) {
            fprintf(stderr, "dlsym(retro_resource_evict_all): %s\n", dlerror());
            dlclose(lib);
            return 1;
        }
    }

    /* blyt_libretro_update_assets: blyt-private dev-mode asset hot-swap (#122). */
    bool (*p_update_assets)(const uint32_t *, size_t) = NULL;
    if (update_assets_after >= 0) {
        p_update_assets =
            (bool (*)(const uint32_t *, size_t))dlsym(lib, "blyt_libretro_update_assets");
        if (!p_update_assets) {
            fprintf(stderr, "dlsym(blyt_libretro_update_assets): %s\n", dlerror());
            dlclose(lib);
            return 1;
        }
    }

    /* blyt_libretro_reload_at: blyt-private dev-mode cart-swap reload (#124/#246). */
    bool (*p_reload_at)(const char *) = NULL;
    if (reload_after >= 0) {
        p_reload_at = (bool (*)(const char *))dlsym(lib, "blyt_libretro_reload_at");
        if (!p_reload_at) {
            fprintf(stderr, "dlsym(blyt_libretro_reload_at): %s\n", dlerror());
            dlclose(lib);
            return 1;
        }
    }

    p_retro_set_environment(env_cb);
    p_retro_set_video_refresh(video_refresh);
    p_retro_set_audio_sample(audio_sample);
    p_retro_set_audio_sample_batch(audio_batch);
    p_retro_set_input_poll(input_poll);
    p_retro_set_input_state(input_state);
    p_retro_init();

    struct retro_game_info game = {cart_path, NULL, 0, NULL};
    if (!p_retro_load_game(&game)) {
        fprintf(stderr, "retro_load_game failed\n");
        p_retro_deinit();
        dlclose(lib);
        return 1;
    }

    int rc = 1;
    for (int frame = 0; frame < MAX_FRAMES; frame++) {
        /* Fire the asset hot-swap before this frame's retro_run so the cart's
         * on_assets_reloaded runs and the same frame already prints v2 (#122). */
        if (p_update_assets && frame == update_assets_after) {
            if (resource_dir_v2)
                setenv("BLYT_RESOURCE_DIR", resource_dir_v2, 1);
            if (!p_update_assets(asset_ids, asset_n))
                fprintf(stderr, "blyt_libretro_update_assets failed\n");
        }
        /* Fire the cart-swap reload before this frame's retro_run so the reloaded
         * cart's init()/on_load_state runs and this frame already prints the new
         * resource content (#124/#246). */
        if (p_reload_at && frame == reload_after) {
            if (!p_reload_at(reload_path))
                fprintf(stderr, "blyt_libretro_reload_at failed\n");
        }
        p_retro_run();
        /* Force-evict after each frame's reads so the *next* frame rehydrates
         * from scratch — the byte-identity / no-cart-visible-change oracle (#137). */
        if (p_evict_all)
            p_evict_all();
        if (p_is_done()) {
            rc = 0;
            break;
        }
        if (run_frames >= 0 && frame + 1 >= run_frames) {
            rc = 0; /* bounded run for a deliberately non-quitting cart */
            break;
        }
        if (p_reset_cycle)
            p_reset_cycle();
    }

    if (rc != 0)
        fprintf(stderr, "cart did not exit after %d frames\n", MAX_FRAMES);

    p_retro_unload_game();
    p_retro_deinit();
    dlclose(lib);
    return rc;
}
