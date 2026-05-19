#include <SDL.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* -------------------------------------------------------------------------
 * Logging
 * ------------------------------------------------------------------------- */

static void RETRO_CALLCONV sdl_log(enum retro_log_level level, const char *fmt, ...) {
    FILE *out = (level >= RETRO_LOG_ERROR) ? stderr : stdout;
    va_list args;
    va_start(args, fmt);
    vfprintf(out, fmt, args);
    va_end(args);
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

static const char *parse_args(int argc, char *argv[], bool *headless) {
    const char *cart = NULL;
    *headless = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--headless") == 0) {
            *headless = true;
        } else if (strcmp(argv[i], "--dump-frame0") == 0 && i + 1 < argc) {
            g_dump_frame0_path = argv[++i];
            *headless = true; /* frame dump implies headless */
        } else if (argv[i][0] != '-') {
            cart = argv[i];
        } else {
            fprintf(stderr, "blytrun: unknown flag: %s\n", argv[i]);
            return NULL;
        }
    }
    return cart;
}

int main(int argc, char *argv[]) {
    bool headless;
    const char *cart_path = parse_args(argc, argv, &headless);
    if (!cart_path) {
        fprintf(stderr, "usage: blytrun [--headless] <cart.blyt>\n");
        return 1;
    }

    retro_set_environment(env_callback);
    retro_set_video_refresh(video_refresh);
    retro_set_audio_sample(audio_sample);
    retro_set_audio_sample_batch(audio_sample_batch);
    retro_set_input_poll(input_poll);
    retro_set_input_state(input_state);
    retro_init();

    struct retro_game_info game = {.path = cart_path, .data = NULL, .size = 0, .meta = NULL};
    if (!retro_load_game(&game)) {
        fprintf(stderr, "blytrun: failed to load cart: %s\n", cart_path);
        retro_deinit();
        return 1;
    }

    struct retro_system_av_info av;
    retro_get_system_av_info(&av);
    double fps = av.timing.fps > 0.0 ? av.timing.fps : 60.0;
    uint32_t frame_ms = (uint32_t)(1000.0 / fps);

    SDL_Window *win = NULL;
    if (!headless) {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
            fprintf(stderr, "blytrun: SDL_Init failed: %s\n", SDL_GetError());
            retro_unload_game();
            retro_deinit();
            return 1;
        }
        g_sdl_ready = true;
        win = SDL_CreateWindow("blyt", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               (int)av.geometry.base_width * 2, (int)av.geometry.base_height * 2,
                               SDL_WINDOW_SHOWN);
        if (!win) {
            fprintf(stderr, "blytrun: SDL_CreateWindow failed: %s\n", SDL_GetError());
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

    while (!g_quit && !blyt_libretro_is_done()) {
        uint32_t t0 = g_sdl_ready ? SDL_GetTicks() : 0;
        retro_run();
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
