#pragma once

#include <stdint.h>

#include "blyt_runtime.h"
#include "state_buffer.h"

/* -------------------------------------------------------------------------
 * Save/load infrastructure (ADR-0125)
 *
 * Save files: $BLYT_SAVE_DIR/<cart_name>/slot_<N>.blys
 * Format: 20-byte header (minor 1) + one CART section per buffer.
 *
 * The on-disk byte layout (magic, header, CART sections) lives in
 * runtime/shared/blyt_blys.{c,h} — the single definition shared with the native
 * bare-metal save path (#129).  save.c only adapts the host's state context and
 * FILE* I/O to it.
 * ------------------------------------------------------------------------- */

typedef struct blyt_session blyt_session_t;

/* Write current state buffers to save slot N.
 * save_dir: path to the save directory for this cart (may be NULL → default).
 * cart_name: used as the subdirectory under save_dir.
 * save_version: the writing cart's .cart.config save_version, stamped into the
 *   header (ADR-0125).
 * Returns BLYT_RUN_OK on success, BLYT_RUN_ERR_IO on failure. */
int blyt_save_write(blyt_state_ctx_t *state, const char *save_dir, const char *cart_name,
                    uint32_t slot, uint32_t save_version);

/* Read save slot N back into state buffers.
 * out_save_version (may be NULL): receives the save_version recorded in the
 *   header by the cart that wrote the save (0 for minor-0 saves; ADR-0087).
 * Returns BLYT_RUN_OK on success, BLYT_RUN_ERR_IO on read failure,
 * BLYT_RUN_ERR_SAVE_SCHEMA on unrecoverable schema mismatch. */
int blyt_save_read(blyt_state_ctx_t *state, const char *save_dir, const char *cart_name,
                   uint32_t slot, uint32_t *out_save_version);
