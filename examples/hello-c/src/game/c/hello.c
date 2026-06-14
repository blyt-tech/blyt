#include "blyt.h"
#include "cart_state.h"
#include <stdio.h>

/* s_frame is deliberately plain static state (not a state buffer) to
 * demonstrate serialising static state in on_save_state/on_load_state. */
static int s_frame;

void blyt_cart_init(void) {
    s_frame = 0;
}

void blyt_cart_on_new_state(void) {
    blyt_buffer_alloc_slot(S_GLOBALS, &(int32_t){-1});
    int32_t slot = -1;
    blyt_buffer_alloc_slot(S_CHARACTER, &slot);
    blyt_buffer_set_u32(S_GLOBALS, 0, S_GLOBALS_PLAYER, blyt_buffer_ref(S_CHARACTER, slot));
    blyt_buffer_set_i32(S_CHARACTER, slot, S_CHARACTER_X, 160);
    blyt_buffer_set_i32(S_CHARACTER, slot, S_CHARACTER_Y, 120);
    blyt_console_debug("init player pos: 160, 120");
}

void blyt_cart_update(void) {
    s_frame++;
    if (s_frame % 10 == 0) {
        blyt_entity_ref_t player = blyt_buffer_get_u32(S_GLOBALS, 0, S_GLOBALS_PLAYER);
        if (blyt_buffer_ref_valid(S_CHARACTER, player)) {
            int32_t slot = blyt_buffer_ref_slot(player);
            int32_t x = (blyt_buffer_get_i32(S_CHARACTER, slot, S_CHARACTER_X) + 1) % 320;
            int32_t y = (blyt_buffer_get_i32(S_CHARACTER, slot, S_CHARACTER_Y) + 1) % 240;
            blyt_buffer_set_i32(S_CHARACTER, slot, S_CHARACTER_X, x);
            blyt_buffer_set_i32(S_CHARACTER, slot, S_CHARACTER_Y, y);
            char buf[64];
            snprintf(buf, sizeof(buf), "update frame %d player pos: %d, %d", s_frame, x, y);
            blyt_console_debug(buf);
        }
    }
}

void blyt_cart_draw(void) {
    if (s_frame % 10 == 0) {
        int32_t slot = blyt_buffer_ref_slot(blyt_buffer_get_u32(S_GLOBALS, 0, S_GLOBALS_PLAYER));
        int32_t x = blyt_buffer_get_i32(S_CHARACTER, slot, S_CHARACTER_X);
        int32_t y = blyt_buffer_get_i32(S_CHARACTER, slot, S_CHARACTER_Y);
        char buf[64];
        snprintf(buf, sizeof(buf), "draw frame %d player pos: %d, %d", s_frame, x, y);
        blyt_console_debug(buf);
    }
}

void blyt_cart_on_save_state(void) {
    blyt_buffer_set_i32(S_GLOBALS, 0, S_GLOBALS_FRAME, s_frame);
}

void blyt_cart_on_load_state(blyt_load_info_t info) {
    (void)info;
    s_frame = blyt_buffer_get_i32(S_GLOBALS, 0, S_GLOBALS_FRAME);
}
