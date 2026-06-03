#pragma once

#include "blyt_runtime.h"
#include "elf32.h"
#include <stddef.h>

/* Runtime API version range (checked against .cart.info api_version). */
#define BLYT_API_VERSION_MAJOR 0
#define BLYT_API_VERSION_MINOR 0

/* .cart.info preamble type tag (ADR-0073) */
#define CART_INFO_TAG "CINF"
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
    int fd;
    void *map;
    size_t map_size;
    char *path; /* heap-allocated copy of the path passed to blyt_cart_open */
    int is_debug;  /* .cart.info `debug` flag (ADR-0129): declared debug build */
    int has_dwarf; /* a .debug_* section is present (DWARF, unstripped) */
};

/* ADR-0129: true for carts built with `blyt build --debug`.
 * is_debug reflects the declared .cart.info flag; has_dwarf reflects actual
 * DWARF presence (the operative signal for source-level debugging). */
int blyt_cart_is_debug(const blyt_cart_t *cart);
int blyt_cart_has_dwarf(const blyt_cart_t *cart);
