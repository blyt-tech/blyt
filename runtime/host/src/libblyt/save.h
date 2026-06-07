#pragma once

#include <stdint.h>

#include "blyt_runtime.h"
#include "state_buffer.h"

/* -------------------------------------------------------------------------
 * Save/load infrastructure (ADR-0125)
 *
 * Save files: $BLYT_SAVE_DIR/<cart_name>/slot_<N>.blys
 * Format: 16-byte header + one CART section per buffer.
 * ------------------------------------------------------------------------- */

/* Save file magic */
#define BLYS_MAGIC "BLYS"
#define BLYS_FORMAT_VERSION_MAJOR UINT16_C(1)
#define BLYS_FORMAT_VERSION_MINOR UINT16_C(0)

/* Section type tag in the chunked body */
#define BLYS_SECT_CART "CART"

typedef struct blyt_session blyt_session_t;

/* Write current state buffers to save slot N.
 * save_dir: path to the save directory for this cart (may be NULL → default).
 * cart_name: used as the subdirectory under save_dir.
 * Returns BLYT_RUN_OK on success, BLYT_RUN_ERR_IO on failure. */
int blyt_save_write(blyt_state_ctx_t *state, const char *save_dir, const char *cart_name,
                    uint32_t slot);

/* Read save slot N back into state buffers.
 * Returns BLYT_RUN_OK on success, BLYT_RUN_ERR_IO on read failure,
 * BLYT_RUN_ERR_SAVE_SCHEMA on unrecoverable schema mismatch. */
int blyt_save_read(blyt_state_ctx_t *state, const char *save_dir, const char *cart_name,
                   uint32_t slot);
