#include <blyt.h>
#include <stdint.h>
#include <stdio.h>

void blyt_debug_frame_pos(const char *who, int32_t frame, int32_t x, int32_t y) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%s frame %d player pos: %d, %d", who, frame, x, y);
    blyt_console_debug(buf);
}
