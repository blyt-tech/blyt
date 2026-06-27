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
 *
 * Text resources are stored with a build-appended trailing NUL (ADR-0088
 * amendment 2026-06-27, #166): the runtime stays byte-blind and pin reports the
 * stored length *including* that NUL.  text_get is the text-aware accessor — it
 * verifies the trailing NUL (its "is this really text" check, since the C
 * typedef gives no compile enforcement) and reports the content length with the
 * NUL stripped.  bytes_get stays byte-exact.
 */

#include "blyt.h"

/* malloc comes from libblytc via the DT_NEEDED chain (ADR-0120); declared here
 * to avoid pulling the full stdlib header into the freestanding guest build. */
extern void *malloc(size_t size);

char *blyt_resource_text_get(blyt_text_resource_t id, size_t *len) {
    const void *ptr = 0;
    size_t size = 0;
    if (blyt_resource_pin(id, &ptr, &size) != BLYT_OK)
        return 0; /* unknown id — nothing pinned, leave *len untouched */

    /* Text invariant (#166): the stored bytes end in the build-appended NUL.
     * A resource without it (a raw resource fed to the text path) is rejected —
     * this is the C error path for a kind mismatch. */
    if (size < 1 || ((const char *)ptr)[size - 1] != '\0') {
        blyt_resource_unpin(id);
        return 0;
    }
    size_t content = size - 1; /* strip the trailing NUL from the reported length */

    /* size bytes = content + the stored NUL, so the copy is already
     * NUL-terminated; no extra byte needed. */
    char *out = (char *)malloc(size);
    if (out) {
        __builtin_memcpy(out, ptr, size);
        if (len)
            *len = content;
    }
    blyt_resource_unpin(id);
    return out;
}

void *blyt_resource_bytes_get(blyt_bytes_resource_t id, size_t *len) {
    const void *ptr = 0;
    size_t size = 0;
    if (blyt_resource_pin(id, &ptr, &size) != BLYT_OK)
        return 0; /* unknown id — nothing pinned, leave *len untouched */

    /* Allocate at least 1 byte so success is always a non-NULL pointer, even for
     * a zero-length resource (no NUL terminator — these are opaque bytes). */
    void *out = malloc(size ? size : 1);
    if (out) {
        if (size)
            __builtin_memcpy(out, ptr, size);
        if (len)
            *len = size;
    }
    blyt_resource_unpin(id);
    return out;
}
