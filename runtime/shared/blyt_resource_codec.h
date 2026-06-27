/*
 * blyt_resource_codec.h — per-resource compression on-disk format + decode
 * (#157, ADR-0026). Part of runtime/shared: the host runtime (blytplay,
 * libretro, WASM — all the same libblyt sources) and the native bare-metal path
 * decode `.cart.resource.<id>` bodies through this one helper so they cannot
 * drift. zstd *decode* is deterministic for a given frame, which is what makes
 * a compressed resource yield bit-identical bytes on every leg.
 *
 * Packed-section layout (little-endian), written by the devtool packer
 * (devtool/src/build/resource_pack.rs — the sole canonical encoder):
 *   off 0  u8    algo      (0 = none, 1 = zstd)
 *   off 1  u8×3  reserved  (zero)
 *   off 4  u32   dsize     decompressed (original) body length in bytes
 *   off 8  ..    body      raw bytes (none) or a zstd frame (zstd)
 *
 * The 8-byte header is always uncompressed, so the runtime reads algo/dsize
 * without touching the body: an uncompressed resource is served zero-copy
 * (data = section + 8) and a compressed one's decompressed size is known before
 * any buffer is allocated (needed by the rest of the epic, #156).
 */

#ifndef BLYT_SHARED_RESOURCE_CODEC_H
#define BLYT_SHARED_RESOURCE_CODEC_H

#include <stddef.h>
#include <stdint.h>

#define BLYT_RES_HEADER_LEN 8u
#define BLYT_RES_ALGO_NONE 0u
#define BLYT_RES_ALGO_ZSTD 1u

/* Parse a packed resource section's 8-byte header. On success writes `algo`,
 * `dsize` (decompressed length), and the body slice (`body`/`body_len`, which is
 * `section_len - 8`), and returns 1. Returns 0 if the section is shorter than
 * the header or carries an unknown algo — callers treat that as malformed. */
int blyt_res_header_parse(const uint8_t *section, size_t section_len, uint8_t *algo,
                          uint32_t *dsize, const uint8_t **body, size_t *body_len);

/* Decode a resource body into `out` (exactly `out_len` bytes, == the header's
 * dsize). For `none`, copies the body verbatim (requires body_len == out_len);
 * for `zstd`, decompresses the frame and verifies the produced length is
 * exactly out_len. Returns 0 on success, -1 on any error (bad algo, size
 * mismatch, corrupt frame). Pure function of its inputs — no allocation. */
int blyt_res_decode(uint8_t algo, const uint8_t *body, size_t body_len, uint8_t *out,
                    size_t out_len);

#endif /* BLYT_SHARED_RESOURCE_CODEC_H */
