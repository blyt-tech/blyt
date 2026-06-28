/*
 * blyt_frame_hash — FNV-1a 64-bit hash of a paletted framebuffer
 * (issue #188, Spike X).  See blyt_frame_hash.h.
 */

#include "blyt_frame_hash.h"

uint64_t blyt_frame_hash(const uint8_t *pixels, size_t n) {
    uint64_t h = UINT64_C(0xcbf29ce484222325); /* FNV offset basis */
    if (!pixels)
        return h;
    for (size_t i = 0; i < n; i++) {
        h ^= (uint64_t)pixels[i];
        h *= UINT64_C(0x100000001b3); /* FNV prime */
    }
    return h;
}
