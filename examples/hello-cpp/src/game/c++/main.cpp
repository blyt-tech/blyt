#include "blyt.h"
#include "cart_state.h"
#include <string>

static int s_frame = 0;
static int32_t s_slot = -1;

extern "C" void blyt_cart_init() {
    int32_t tmp = -1;
    blyt_buffer_alloc_slot(S_GLOBALS, &tmp);
    blyt_buffer_alloc_slot(S_PLAYER, &s_slot);
    blyt_buffer_set_i32(S_PLAYER, s_slot, S_PLAYER_X, 0);
    blyt_buffer_set_i32(S_PLAYER, s_slot, S_PLAYER_Y, 0);
    blyt_console_debug("init player pos: 0, 0");
}

extern "C" void blyt_cart_update() {
    ++s_frame;
    if (s_frame % 10 == 0) {
        int32_t x = (blyt_buffer_get_i32(S_PLAYER, s_slot, S_PLAYER_X) + 1) % 320;
        int32_t y = (blyt_buffer_get_i32(S_PLAYER, s_slot, S_PLAYER_Y) + 1) % 240;
        blyt_buffer_set_i32(S_PLAYER, s_slot, S_PLAYER_X, x);
        blyt_buffer_set_i32(S_PLAYER, s_slot, S_PLAYER_Y, y);
        blyt_console_debug(("update frame " + std::to_string(s_frame) +
                            " player pos: " + std::to_string(x) +
                            ", " + std::to_string(y)).c_str());
    }
}

extern "C" void blyt_cart_draw() {
    if (s_frame % 10 == 0) {
        int32_t x = blyt_buffer_get_i32(S_PLAYER, s_slot, S_PLAYER_X);
        int32_t y = blyt_buffer_get_i32(S_PLAYER, s_slot, S_PLAYER_Y);
        blyt_console_debug(("draw frame " + std::to_string(s_frame) +
                            " player pos: " + std::to_string(x) +
                            ", " + std::to_string(y)).c_str());
    }
}

extern "C" void blyt_cart_on_save_state() {
    blyt_buffer_set_i32(S_GLOBALS, 0, S_GLOBALS_FRAME, s_frame);
}

extern "C" void blyt_cart_on_load_state(blyt_load_info_t info) {
    (void)info;
    s_frame = blyt_buffer_get_i32(S_GLOBALS, 0, S_GLOBALS_FRAME);
    s_slot = 0;
}
