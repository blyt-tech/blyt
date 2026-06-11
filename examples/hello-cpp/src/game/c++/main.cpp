#include "blyt.h"
#include "cart_state.h"
#include <string>

// s_frame is deliberately plain static state (not a state buffer) to
// demonstrate serialising static state in on_save_state/on_load_state.
static int s_frame;

extern "C" void blyt_cart_init() {
    s_frame = 0;
}

extern "C" void blyt_cart_on_new_state() {
    int32_t tmp = -1;
    blyt_buffer_alloc_slot(S_GLOBALS, &tmp);
    int32_t slot = -1;
    blyt_buffer_alloc_slot(S_CHARACTER, &slot);
    blyt_buffer_set_i32(S_GLOBALS, 0, S_GLOBALS_PLAYER_ID, slot);
    blyt_buffer_set_i32(S_CHARACTER, slot, S_CHARACTER_X, 160);
    blyt_buffer_set_i32(S_CHARACTER, slot, S_CHARACTER_Y, 120);
    blyt_console_debug("init player pos: 160, 120");
}

extern "C" void blyt_cart_update() {
    ++s_frame;
    if (s_frame % 10 == 0) {
        int32_t slot = blyt_buffer_get_i32(S_GLOBALS, 0, S_GLOBALS_PLAYER_ID);
        int32_t x = (blyt_buffer_get_i32(S_CHARACTER, slot, S_CHARACTER_X) + 1) % 320;
        int32_t y = (blyt_buffer_get_i32(S_CHARACTER, slot, S_CHARACTER_Y) + 1) % 240;
        blyt_buffer_set_i32(S_CHARACTER, slot, S_CHARACTER_X, x);
        blyt_buffer_set_i32(S_CHARACTER, slot, S_CHARACTER_Y, y);
        blyt_console_debug(("update frame " + std::to_string(s_frame) +
                            " player pos: " + std::to_string(x) +
                            ", " + std::to_string(y)).c_str());
    }
}

extern "C" void blyt_cart_draw() {
    if (s_frame % 10 == 0) {
        int32_t slot = blyt_buffer_get_i32(S_GLOBALS, 0, S_GLOBALS_PLAYER_ID);
        int32_t x = blyt_buffer_get_i32(S_CHARACTER, slot, S_CHARACTER_X);
        int32_t y = blyt_buffer_get_i32(S_CHARACTER, slot, S_CHARACTER_Y);
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
}
