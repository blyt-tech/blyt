#include <SDL.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blyt_runtime.h"

static void log_callback(const char *msg) {
    printf("%s\n", msg);
}

/*
 * Per-frame callback for the SDL frontend.  Called by blyt_cart_run after
 * each blyt_cart_draw(); polls SDL events and caps the frame rate.
 * userdata is the SDL_Window* (currently unused; reserved for future use).
 */
static void sdl_frame_callback(void *userdata) {
    (void)userdata;
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) {
            /* Signal the cart to finish on the next iteration.
             * We can't halt the emulator directly here, but the cart's
             * on_quit default calls blyt_quit_ready() which exits the loop. */
        }
    }
    /* Cap to ~60 fps; the cart drives timing via blyt_frame_done(). */
    SDL_Delay(16);
}

/*
 * Parse argv for the cart path and --headless flag.
 * Returns the cart path, or NULL if usage is wrong.
 * Sets *headless to true if --headless is present.
 */
static const char *parse_args(int argc, char *argv[], bool *headless) {
    const char *cart = NULL;
    *headless = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--headless") == 0) {
            *headless = true;
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

    blyt_cart_t *cart = NULL;
    blyt_cart_err_t load_err = blyt_cart_open(cart_path, &cart);
    if (load_err != BLYT_CART_OK) {
        fprintf(stderr, "blytrun: failed to load cart: %s\n", blyt_cart_err_str(load_err));
        return 1;
    }

    if (headless) {
        blyt_cart_run_err_t run_err = blyt_cart_run(cart, log_callback, NULL, NULL);
        blyt_cart_close(cart);
        if (run_err == BLYT_RUN_ERR_ECALL_TRAP) {
            fprintf(stderr, "blytrun: cart attempted a non-permitted syscall\n");
            return 1;
        }
        if (run_err == BLYT_RUN_ERR_ABORT) {
            fprintf(stderr, "blytrun: cart aborted (fatal internal error)\n");
            return 1;
        }
        return (run_err == BLYT_RUN_OK) ? 0 : 1;
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "blytrun: SDL_Init failed: %s\n", SDL_GetError());
        blyt_cart_close(cart);
        return 1;
    }

    SDL_Window *win = SDL_CreateWindow("blyt", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 640,
                                       480, SDL_WINDOW_SHOWN);
    if (!win) {
        fprintf(stderr, "blytrun: SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        blyt_cart_close(cart);
        return 1;
    }

    /* Run the cart with the SDL frame callback.  sdl_frame_callback is called
     * once per update+draw cycle; it polls events and paces the frame rate.
     * blyt_cart_run returns only when the cart exits (blyt_quit_ready). */
    blyt_cart_run_err_t run_err = blyt_cart_run(cart, log_callback, sdl_frame_callback, win);

    if (run_err == BLYT_RUN_ERR_ECALL_TRAP) {
        fprintf(stderr, "blytrun: cart attempted a non-permitted syscall\n");
    } else if (run_err == BLYT_RUN_ERR_ABORT) {
        fprintf(stderr, "blytrun: cart aborted (fatal internal error)\n");
    } else if (run_err != BLYT_RUN_OK) {
        fprintf(stderr, "blytrun: cart run failed (err=%d)\n", (int)run_err);
    }

    SDL_DestroyWindow(win);
    SDL_Quit();
    blyt_cart_close(cart);
    return (run_err == BLYT_RUN_OK) ? 0 : 1;
}
