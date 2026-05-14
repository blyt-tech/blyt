#pragma once

#include "blyt_runtime.h"
#include "elf32.h"
#include <stddef.h>

/* Runtime API version range (checked against .cart.info api_version). */
#define BLYT_API_VERSION_MAJOR 0
#define BLYT_API_VERSION_MINOR 0
#define BLYT_API_VERSION_STR   "0.0"

/* .cart.info preamble type tag (ADR-0073) */
#define CART_INFO_TAG   "CINF"
/* .cart.config preamble type tag (ADR-0073) */
#define CART_CONFIG_TAG "CCFG"

/* Preamble size prepended to every FlatBuffers section (ADR-0073):
 *   offset 0: 4-byte ASCII type tag
 *   offset 4: uint16 LE format major version
 *   offset 6: uint16 LE format minor version
 *   offset 8: FlatBuffers buffer
 */
#define SECT_PREAMBLE_SIZE 8u

struct blyt_cart {
    int    fd;
    void  *map;
    size_t map_size;
};
