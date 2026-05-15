#include <SDL.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blyt_runtime.h"

static void log_callback(const char *msg) {
    fprintf(stderr, "[cart] %s\n", msg);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "usage: blytrun <cart.blyt>\n");
        return 1;
    }

    blyt_cart_t *cart = NULL;
    blyt_cart_err_t load_err = blyt_cart_open(argv[1], &cart);
    if (load_err != BLYT_CART_OK) {
        fprintf(stderr, "blytrun: failed to load cart: %s\n", blyt_cart_err_str(load_err));
        return 1;
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

    /* Run the cart in the emulator. The cart may produce console debug output.
     * We process pending events so the window stays responsive. */
    blyt_cart_run_err_t run_err = blyt_cart_run(cart, log_callback);

    if (run_err == BLYT_RUN_ERR_ECALL_TRAP) {
        fprintf(stderr, "blytrun: cart attempted a non-permitted syscall\n");
    } else if (run_err != BLYT_RUN_OK) {
        fprintf(stderr, "blytrun: cart run failed (err=%d)\n", (int)run_err);
    }

    /* Drain events so the window reacts if the user closes it */
    SDL_Event ev;
    bool quit = (run_err != BLYT_RUN_OK);
    while (!quit) {
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT)
                quit = true;
        }
        SDL_Delay(16);
        /* No game loop yet — quit once the cart has returned from blyt_main */
        quit = true;
    }

    SDL_DestroyWindow(win);
    SDL_Quit();
    blyt_cart_close(cart);
    return (run_err == BLYT_RUN_OK) ? 0 : 1;
}
