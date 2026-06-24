/*
 * blyt_gen.h — entity-slot generation-counter wrap primitive (ADR-0096).
 *
 * Part of runtime/shared (freestanding; see blyt_fp_canon.h header comment).
 *
 * A state-buffer slot carries a u16 generation counter, bumped on each
 * successful free_slot so a stale packed ref (gen<<16 | slot) is detectable.
 * Generation 0 is reserved as the invalid sentinel (BLYT_ENTITY_REF_NONE == 0),
 * so the counter wraps 65535 -> 1, never to 0.
 *
 * This primitive operates on the UNBIASED, public generation value (1..65535).
 * Callers that store the counter with a representation bias apply it at their
 * own storage boundary — e.g. the native path stores (generation - 1) so its
 * BSS-zero default is generation 1 with no init code, and converts in/out of
 * the public value around this call.  Keeping the bias out of the primitive is
 * what lets host and native share it without changing either side's on-disk
 * representation (issue #128).
 */

#ifndef BLYT_SHARED_GEN_H
#define BLYT_SHARED_GEN_H

#include <stdint.h>

/* Next public generation after `gen`, wrapping 65535 -> 1 (0 stays reserved). */
static inline uint16_t blyt_gen_next(uint16_t gen) {
    return (uint16_t)(gen == UINT16_C(0xFFFF) ? 1u : (uint32_t)gen + 1u);
}

#endif /* BLYT_SHARED_GEN_H */
