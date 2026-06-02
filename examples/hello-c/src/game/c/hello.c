#include "blyt.h"
#include <stdio.h>

static int s_frame = 0;

void blyt_cart_init(void) {
    blyt_console_debug("hello from c");
}

void blyt_cart_update(void) {
    char buf[32];
    s_frame++;
    if (s_frame % 60 == 0) {
        snprintf(buf, sizeof(buf), "update %d", s_frame);
        blyt_console_debug(buf);
    }
}

void blyt_cart_draw(void) {
    char buf[32];
    if (s_frame % 60 == 0) {
        snprintf(buf, sizeof(buf), "draw %d", s_frame);
        blyt_console_debug(buf);
    }
}
