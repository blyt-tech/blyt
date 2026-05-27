#include "blyt.h"

static int s_frame = 0;

void blyt_cart_init(void) {
    blyt_console_debug("init");
}

void blyt_cart_update(void) {
    s_frame++;
    if (s_frame % 60 == 0) {
        blyt_console_debug("update");
    }
}

void blyt_cart_draw(void) {
    if (s_frame % 60 == 0) {
        blyt_console_debug("draw");
    }
}
