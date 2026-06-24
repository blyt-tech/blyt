/*
 * libblyt32 — Blyt32 variant shared library, native RISC-V path.
 *
 * Intentionally empty for now (issue #128).  The Blyt32 variant carries only
 * variant-specific APIs — graphics (resolution/palette/format tied to the
 * variant) — none of which are implemented yet, so this library currently
 * defines no symbols.  The variant-agnostic real-work impls (state buffers,
 * save/load, frame-boundary FP determinism, console debug, startup, exit) live
 * in the native libblytcommon variant (frontends/native/src/libblytcommon/).
 *
 * The .so is still built and shipped: it is the cart's direct DT_NEEDED and
 * carries DT_NEEDED libblytcommon.so + libblytc.so so the cart resolves the
 * relocated symbols and the system C library over the chain to ld-blyt.so.1.
 * Graphics implementations will land here when introduced.
 */

/* A file-scope declaration keeps this a valid (non-empty) translation unit
 * without emitting any symbol into the library. */
typedef int blyt32_native_variant_placeholder;
