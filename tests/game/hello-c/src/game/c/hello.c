#include "blyt.h"

static int s_frame = 0;

void blyt_cart_init(void) {
    blyt_console_debug("init");
}

void blyt_cart_update(void) {
    blyt_console_debug("update");
    s_frame++;
    if (s_frame >= 2) {
        blyt_quit();
    }
}

void blyt_cart_draw(void) {
    blyt_console_debug("draw");
}
