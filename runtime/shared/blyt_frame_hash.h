#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * blyt_frame_hash — deterministic 64-bit hash of a paletted framebuffer
 * (issue #188, Spike X).
 *
 * FNV-1a over the raw palette-index bytes.  Integer-only (the 64-bit multiply
 * wraps mod 2^64, which is fully defined and identical on every compile
 * target), so it is the cross-target determinism probe for Q2: each execution
 * leg hashes its own blyt_session_get_pixels() buffer and the values must match
 * bit-for-bit.  Lives in runtime/shared so the host runtime and the native
 * libblyt32 variant compute it from one source.
 */
uint64_t blyt_frame_hash(const uint8_t *pixels, size_t n);
