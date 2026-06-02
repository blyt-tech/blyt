#include "blyt.h"
#include <string>

static int s_frame = 0;

extern "C" void blyt_cart_init() {
    blyt_console_debug("hello from c++");
}

extern "C" void blyt_cart_update() {
    ++s_frame;
    if (s_frame % 60 == 0)
        blyt_console_debug(("update " + std::to_string(s_frame)).c_str());
}

extern "C" void blyt_cart_draw() {
    if (s_frame % 60 == 0)
        blyt_console_debug(("draw " + std::to_string(s_frame)).c_str());
}
