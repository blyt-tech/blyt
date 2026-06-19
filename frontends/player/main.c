#include <SDL.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h> /* stat, S_ISDIR */
#include <unistd.h> /* setenv, readlink */
#ifdef __APPLE__
#include <mach-o/dyld.h> /* _NSGetExecutablePath */
#endif

#include "blyt_runtime.h"
#include "libretro.h"

/* blyt_libretro.c is compiled into this binary (without BLYT_EMBED_LIBS),
 * so dynlink falls back to BLYT_LIB_DIR for the guest runtime libraries. */

/* -------------------------------------------------------------------------
 * Forward declarations of the retro_* functions from blyt_libretro.c
 * ------------------------------------------------------------------------- */

void retro_set_environment(retro_environment_t cb);
void retro_set_video_refresh(retro_video_refresh_t cb);
void retro_set_audio_sample(retro_audio_sample_t cb);
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb);
void retro_set_input_poll(retro_input_poll_t cb);
void retro_set_input_state(retro_input_state_t cb);
void retro_init(void);
void retro_deinit(void);
void retro_get_system_av_info(struct retro_system_av_info *info);
bool retro_load_game(const struct retro_game_info *game);
void retro_unload_game(void);
void retro_run(void);

/* Used only by the SDL frontend (direct link) for loop termination and exit
 * status — not part of the standard libretro interface. */
bool blyt_libretro_is_done(void);
blyt_cart_run_err_t blyt_libretro_run_err(void);
void retro_reset_every_frame_cycle(void);
#ifdef BLYT_DAP
bool blyt_libretro_dap_wait_ready(void);
#endif
#ifdef BLYT_GDB
bool blyt_libretro_gdb_wait_attached(void);
void blyt_libretro_gdb_continue_initial_halt(void);
#endif

/* -------------------------------------------------------------------------
 * Logging
 * ------------------------------------------------------------------------- */

static void RETRO_CALLCONV sdl_log(enum retro_log_level level, const char *fmt, ...) {
    FILE *out = (level >= RETRO_LOG_ERROR) ? stderr : stdout;
    va_list args;
    va_start(args, fmt);
    vfprintf(out, fmt, args);
    va_end(args);
    fflush(out);
}

/* -------------------------------------------------------------------------
 * Environment callback
 * ------------------------------------------------------------------------- */

static bool env_callback(unsigned cmd, void *data) {
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: {
        struct retro_log_callback *log = (struct retro_log_callback *)data;
        log->log = sdl_log;
        return true;
    }
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        return true; /* noted; ignored until graphics land */
    default:
        return false;
    }
}

/* -------------------------------------------------------------------------
 * Libretro callbacks
 * ------------------------------------------------------------------------- */

static SDL_Renderer *g_renderer = NULL;
static SDL_Texture *g_texture = NULL;

/* --dump-frame0: write first XRGB8888 frame as raw bytes then exit. */
static const char *g_dump_frame0_path = NULL;
static bool g_quit; /* forward declaration — defined below */

static void video_refresh(const void *data, unsigned width, unsigned height, size_t pitch) {
    if (g_dump_frame0_path && data) {
        FILE *f = fopen(g_dump_frame0_path, "wb");
        if (f) {
            for (unsigned row = 0; row < height; row++)
                fwrite((const char *)data + row * pitch, 4, width, f);
            fclose(f);
        }
        g_dump_frame0_path = NULL; /* only dump once */
        g_quit = true;
        return;
    }
    if (!data || !g_renderer || !g_texture)
        return;
    SDL_UpdateTexture(g_texture, NULL, data, (int)pitch);
    SDL_RenderClear(g_renderer);
    SDL_RenderCopy(g_renderer, g_texture, NULL, NULL);
    SDL_RenderPresent(g_renderer);
}

static void audio_sample(int16_t left, int16_t right) {
    (void)left;
    (void)right;
}

static size_t audio_sample_batch(const int16_t *data, size_t frames) {
    (void)data;
    return frames;
}

static bool g_quit = false;
static bool g_sdl_ready = false;

static void input_poll(void) {
    if (!g_sdl_ready)
        return;
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT)
            g_quit = true;
    }
}

static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id) {
    (void)port;
    (void)device;
    (void)index;
    (void)id;
    return 0;
}

/* -------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */

static int g_dap_port = -1; /* -1 = disabled, 0 = OS-assigned, >0 = fixed */
static int g_gdb_port = -1; /* -1 = disabled, 0 = OS-assigned, >0 = fixed */
static const char *g_trace = NULL; /* --trace: BLYT_TRACE channel list */
static int g_quit_after = -1; /* -1 = disabled; >=0 = exit after N frames */
static bool g_reset_every_frame =
    false; /* --reset-every-frame: run a save/clear/restore cycle after each frame */

/* If BLYT_LIB_DIR is unset, try to infer it as <binary_dir>/../lib.
 * This lets blytplay/blytdebug work without any environment setup when
 * installed in an SDK whose layout is bin/ and lib/ side by side.
 * When prefer_debug is true (DAP or GDB enabled), prefer lib/debug/ if it
 * contains libblyt32.so — that variant includes the DAP ECALL hook code. */
static void maybe_setenv_lib_dir(bool prefer_debug) {
    if (getenv("BLYT_LIB_DIR") && getenv("BLYT_LIB_DIR")[0] != '\0')
        return;

    char exe[4096];
#ifdef __APPLE__
    uint32_t size = sizeof(exe);
    if (_NSGetExecutablePath(exe, &size) != 0)
        return;
#else
    ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (len <= 0)
        return;
    exe[len] = '\0';
#endif

    char resolved[4096];
    if (!realpath(exe, resolved))
        return;

    /* resolved = /path/to/bin/blytplay; go up two levels: dirname + "/../lib" */
    char *slash = strrchr(resolved, '/');
    if (!slash)
        return;
    *slash = '\0'; /* resolved is now the bin/ directory */

    char lib_dir[4096];
    char lib_dir_real[4096];
    char probe[4096];
    int n;

    /* When running with DAP or GDB enabled, prefer lib/debug/ — it contains the
     * guest libraries built with BLYT_DAP=1 (master_hook_ecall ECALL stubs). */
    if (prefer_debug) {
        n = snprintf(lib_dir, sizeof(lib_dir), "%s/../lib/debug", resolved);
        if (n > 0 && (size_t)n < sizeof(lib_dir) && realpath(lib_dir, lib_dir_real)) {
            n = snprintf(probe, sizeof(probe), "%s/libblyt32.so", lib_dir_real);
            if (n > 0 && (size_t)n < sizeof(probe) && access(probe, R_OK) == 0) {
                setenv("BLYT_LIB_DIR", lib_dir_real, /* overwrite */ 0);
                return;
            }
        }
    }

    n = snprintf(lib_dir, sizeof(lib_dir), "%s/../lib", resolved);
    if (n <= 0 || (size_t)n >= sizeof(lib_dir))
        return;

    if (!realpath(lib_dir, lib_dir_real))
        return;

    /* Verify libblyt32.so is present before committing. */
    n = snprintf(probe, sizeof(probe), "%s/libblyt32.so", lib_dir_real);
    if (n <= 0 || (size_t)n >= sizeof(probe))
        return;
    if (access(probe, R_OK) != 0)
        return;

    setenv("BLYT_LIB_DIR", lib_dir_real, /* overwrite */ 0);
}

static const char *parse_args(int argc, char *argv[], bool *headless) {
    const char *cart = NULL;
    *headless = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--headless") == 0) {
            *headless = true;
        } else if (strcmp(argv[i], "--dump-frame0") == 0 && i + 1 < argc) {
            g_dump_frame0_path = argv[++i];
            *headless = true; /* frame dump implies headless */
        } else if (strcmp(argv[i], "--debug") == 0) {
            /* Optional port number; defaults to 0 (OS-assigned) */
            if (i + 1 < argc && argv[i + 1][0] != '-')
                g_dap_port = atoi(argv[++i]);
            else
                g_dap_port = 0;
        } else if (strcmp(argv[i], "--gdb") == 0) {
            /* Optional port number; defaults to 0 (OS-assigned) */
            if (i + 1 < argc && argv[i + 1][0] != '-')
                g_gdb_port = atoi(argv[++i]);
            else
                g_gdb_port = 0;
        } else if (strcmp(argv[i], "--quit-after") == 0 && i + 1 < argc) {
            g_quit_after = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--trace") == 0 && i + 1 < argc) {
            g_trace = argv[++i];
        } else if (strncmp(argv[i], "--trace=", 8) == 0) {
            g_trace = argv[i] + 8;
        } else if (strcmp(argv[i], "--reset-every-frame") == 0) {
            g_reset_every_frame = true;
        } else if (argv[i][0] != '-') {
            cart = argv[i];
        } else {
            fprintf(stderr, "blytplay: unknown flag: %s\n", argv[i]);
            return NULL;
        }
    }
    return cart;
}

int main(int argc, char *argv[]) {
    bool headless;
    const char *cart_path = parse_args(argc, argv, &headless);
    if (!cart_path) {
        fprintf(stderr, "usage: blytplay [--headless] <cart.blyt>\n");
        return 1;
    }

    bool debug_mode = (g_dap_port >= 0 || g_gdb_port >= 0);

    /* Project-dir mode: if the argument is a directory, derive the dev ELF
     * path (build/.elf for release, build/.dbg.elf for debug).  The dev ELF
     * is produced by `blyt run`/`blyt debug` and is a valid ELF with all
     * required sections — no changes to the cart loading path. */
    char elf_path_buf[4096];
    {
        struct stat st;
        if (stat(cart_path, &st) == 0 && S_ISDIR(st.st_mode)) {
            const char *elf_name = debug_mode ? ".dbg.elf" : ".elf";
            int n =
                snprintf(elf_path_buf, sizeof(elf_path_buf), "%s/build/%s", cart_path, elf_name);
            if (n <= 0 || (size_t)n >= sizeof(elf_path_buf)) {
                fprintf(stderr, "blytplay: project path too long\n");
                return 1;
            }
            cart_path = elf_path_buf;
        }
    }

    maybe_setenv_lib_dir(debug_mode);

    /* --trace is just the flag form of the BLYT_TRACE env var (same pattern
     * as --debug → BLYT_DAP_PORT); set it before retro_init(). */
    if (g_trace)
        setenv("BLYT_TRACE", g_trace, 1);

#ifdef BLYT_DAP
    if (g_dap_port >= 0) {
        char portbuf[16];
        snprintf(portbuf, sizeof(portbuf), "%d", g_dap_port);
        setenv("BLYT_DAP_PORT", portbuf, 1);
    }
#endif
#ifdef BLYT_GDB
    if (g_gdb_port >= 0) {
        char portbuf[16];
        snprintf(portbuf, sizeof(portbuf), "%d", g_gdb_port);
        setenv("BLYT_GDB_PORT", portbuf, 1);
    }
#endif

    retro_set_environment(env_callback);
    retro_set_video_refresh(video_refresh);
    retro_set_audio_sample(audio_sample);
    retro_set_audio_sample_batch(audio_sample_batch);
    retro_set_input_poll(input_poll);
    retro_set_input_state(input_state);
    retro_init();

    struct retro_game_info game = {.path = cart_path, .data = NULL, .size = 0, .meta = NULL};
    if (!retro_load_game(&game)) {
        fprintf(stderr, "blytplay: failed to load cart: %s\n", cart_path);
        retro_deinit();
        return 1;
    }

    struct retro_system_av_info av;
    retro_get_system_av_info(&av);
    double fps = av.timing.fps > 0.0 ? av.timing.fps : 60.0;
    uint32_t frame_ms = (uint32_t)(1000.0 / fps);

#ifdef BLYT_DAP
    /* Wait for the DAP client to finish configuration (setBreakpoints +
     * configurationDone) before starting the game loop, so that breakpoints
     * are registered before init() executes. */
    if (g_dap_port >= 0)
        blyt_libretro_dap_wait_ready();
#endif
#ifdef BLYT_GDB
    /* Wait for a GDB client to connect before running the cart, so that the
     * client can set breakpoints before blyt_cart_init() executes. */
    if (g_gdb_port >= 0)
        blyt_libretro_gdb_wait_attached();
#endif
#if defined(BLYT_DAP) && defined(BLYT_GDB)
    /* Hybrid mode (both DAP and GDB active): Lua breakpoints are already
     * registered (DAP configurationDone received above).  Clear the GDB
     * initial halt so the cart can run immediately — we don't need lldb-dap
     * to send vCont;c before Lua code executes. */
    if (g_dap_port >= 0 && g_gdb_port >= 0)
        blyt_libretro_gdb_continue_initial_halt();
#endif

    SDL_Window *win = NULL;
    if (!headless) {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
            fprintf(stderr, "blytplay: SDL_Init failed: %s\n", SDL_GetError());
            retro_unload_game();
            retro_deinit();
            return 1;
        }
        g_sdl_ready = true;
        win = SDL_CreateWindow("blyt", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               (int)av.geometry.base_width * 2, (int)av.geometry.base_height * 2,
                               SDL_WINDOW_SHOWN);
        if (!win) {
            fprintf(stderr, "blytplay: SDL_CreateWindow failed: %s\n", SDL_GetError());
            SDL_Quit();
            retro_unload_game();
            retro_deinit();
            return 1;
        }
        g_renderer = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
        if (g_renderer)
            g_texture =
                SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING,
                                  (int)av.geometry.base_width, (int)av.geometry.base_height);
    }

    int frame_count = 0;
    while (!g_quit && !blyt_libretro_is_done()) {
        uint32_t t0 = g_sdl_ready ? SDL_GetTicks() : 0;
        retro_run();
        if (g_quit_after >= 0 && ++frame_count >= g_quit_after)
            g_quit = true;
        if (g_reset_every_frame && !g_quit)
            retro_reset_every_frame_cycle();
        if (g_sdl_ready) {
            uint32_t elapsed = SDL_GetTicks() - t0;
            if (elapsed < frame_ms)
                SDL_Delay(frame_ms - elapsed);
        }
    }

    if (win) {
        if (g_texture)
            SDL_DestroyTexture(g_texture);
        if (g_renderer)
            SDL_DestroyRenderer(g_renderer);
        SDL_DestroyWindow(win);
        SDL_Quit();
    }

    blyt_cart_run_err_t run_err = blyt_libretro_run_err();
    retro_unload_game();
    retro_deinit();
    return (run_err == BLYT_RUN_OK) ? 0 : 1;
}
