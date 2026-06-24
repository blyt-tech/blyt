/*
 * resources.c — portable resource convenience helpers (libblytcommon, #123).
 *
 * Variant-agnostic: built once and compiled into every libblytcommon variant
 * (emulated + native).  blyt_resource_text_get is a thin wrapper over the
 * pin/unpin primitives — pin yields a frame-scoped view of the bytes, we copy
 * that into an owned, NUL-terminated buffer the caller keeps, then unpin.  The
 * primitives themselves are variant-specific (ECALL on emulated, real work on
 * native), so this helper works on every target without change and needs no
 * ECALL of its own.
 */

#include "blyt.h"

/* malloc comes from libblytc via the DT_NEEDED chain (ADR-0120); declared here
 * to avoid pulling the full stdlib header into the freestanding guest build. */
extern void *malloc(size_t size);

char *blyt_resource_text_get(blyt_resource_id_t id, size_t *len) {
    const void *ptr = 0;
    size_t size = 0;
    if (blyt_resource_pin(id, &ptr, &size) != BLYT_OK)
        return 0; /* unknown id — nothing pinned, leave *len untouched */

    char *out = (char *)malloc(size + 1);
    if (out) {
        if (size)
            __builtin_memcpy(out, ptr, size);
        out[size] = '\0'; /* NUL-terminate for text convenience; size is authoritative */
        if (len)
            *len = size;
    }
    blyt_resource_unpin(id);
    return out;
}
