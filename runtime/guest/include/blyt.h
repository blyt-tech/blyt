#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Common result type
 * ------------------------------------------------------------------------- */
typedef enum {
    BLYT_OK = 0,
    BLYT_ERR_INVALID_ARG = 1,
    BLYT_ERR_BUFFER_FULL = 2,
    BLYT_ERR_IO = 3,
    BLYT_ERR_NOT_FOUND = 4,
    BLYT_ERR_SCHEMA_MISMATCH = 5,
} blyt_result_t;

/* -------------------------------------------------------------------------
 * State buffer types (ADR-0009, ADR-0057)
 *
 * blyt_buffer_h: 1-based buffer pool handle (packer-generated constant).
 * blyt_field_h:  packed uint32_t — high 16 bits = buffer_id, low 16 bits =
 *                field index within the buffer's flattened record.
 * blyt_field_index() extracts the field index; the runtime uses it to
 * index into per-field SOA arrays.
 * BLYT_FIELD_NONE (0) is the invalid-field sentinel.
 * BLYT_INVALID_SLOT (-1) is returned by blyt_buffer_alloc_slot when full.
 * ------------------------------------------------------------------------- */
typedef uint32_t blyt_buffer_h;
typedef uint32_t blyt_field_h;
#define BLYT_FIELD_NONE ((blyt_field_h)0)
#define BLYT_FIELD_INDEX(fh) ((uint32_t)((fh) & 0xFFFFu))
#define BLYT_INVALID_SLOT ((int32_t)-1)

/* -------------------------------------------------------------------------
 * State buffer API (ADR-0009, ADR-0010, ADR-0057, ADR-0058)
 *
 * buf:   packer-generated blyt_buffer_h constant (e.g. S_PLAYERS)
 * slot:  0-based entity slot within the buffer (0..count-1)
 * field: packer-generated blyt_field_h constant (e.g. S_PLAYER_HP)
 *
 * f32 writes NaN-canonicalize to 0x7FC00000 (ADR-0010).
 * All functions are no-ops on invalid buf/slot/field; debug builds assert.
 * ------------------------------------------------------------------------- */
float blyt_buffer_get_f32(blyt_buffer_h buf, int32_t slot, blyt_field_h field);
void blyt_buffer_set_f32(blyt_buffer_h buf, int32_t slot, blyt_field_h field, float v);
double blyt_buffer_get_f64(blyt_buffer_h buf, int32_t slot, blyt_field_h field);
void blyt_buffer_set_f64(blyt_buffer_h buf, int32_t slot, blyt_field_h field, double v);
int32_t blyt_buffer_get_i32(blyt_buffer_h buf, int32_t slot, blyt_field_h field);
void blyt_buffer_set_i32(blyt_buffer_h buf, int32_t slot, blyt_field_h field, int32_t v);
uint32_t blyt_buffer_get_u32(blyt_buffer_h buf, int32_t slot, blyt_field_h field);
void blyt_buffer_set_u32(blyt_buffer_h buf, int32_t slot, blyt_field_h field, uint32_t v);
int16_t blyt_buffer_get_i16(blyt_buffer_h buf, int32_t slot, blyt_field_h field);
void blyt_buffer_set_i16(blyt_buffer_h buf, int32_t slot, blyt_field_h field, int16_t v);
uint16_t blyt_buffer_get_u16(blyt_buffer_h buf, int32_t slot, blyt_field_h field);
void blyt_buffer_set_u16(blyt_buffer_h buf, int32_t slot, blyt_field_h field, uint16_t v);
int8_t blyt_buffer_get_i8(blyt_buffer_h buf, int32_t slot, blyt_field_h field);
void blyt_buffer_set_i8(blyt_buffer_h buf, int32_t slot, blyt_field_h field, int8_t v);
uint8_t blyt_buffer_get_u8(blyt_buffer_h buf, int32_t slot, blyt_field_h field);
void blyt_buffer_set_u8(blyt_buffer_h buf, int32_t slot, blyt_field_h field, uint8_t v);
bool blyt_buffer_get_bool(blyt_buffer_h buf, int32_t slot, blyt_field_h field);
void blyt_buffer_set_bool(blyt_buffer_h buf, int32_t slot, blyt_field_h field, bool v);

/* Slot lifecycle (ADR-0058) */
blyt_result_t blyt_buffer_alloc_slot(blyt_buffer_h buf, int32_t *out_slot);
blyt_result_t blyt_buffer_free_slot(blyt_buffer_h buf, int32_t slot);

/* -------------------------------------------------------------------------
 * Packed entity refs (ADR-0096)
 *
 * A blyt_entity_ref_t stores a slot's generation counter in the high 16 bits
 * and the slot index in the low 16 bits; BLYT_ENTITY_REF_NONE (0) is the
 * null/invalid sentinel.  Generations start at 1 and are bumped when a slot
 * is freed, so a ref taken before free_slot is detectably stale after the
 * slot is reused.  Refs are plain u32 values: store them in u32 state-buffer
 * fields (manifest `ref:` fields) and they serialize with the buffer.
 *
 * blyt_buffer_ref returns the ref for an allocated slot, NONE otherwise.
 * blyt_buffer_ref_valid checks that ref still names the same entity.
 * blyt_buffer_ref_slot extracts the slot index; only meaningful when the
 * ref is valid (ref_slot(NONE) == 0).
 * ------------------------------------------------------------------------- */
typedef uint32_t blyt_entity_ref_t;
#define BLYT_ENTITY_REF_NONE ((blyt_entity_ref_t)0)

blyt_entity_ref_t blyt_buffer_ref(blyt_buffer_h buf, int32_t slot);
bool blyt_buffer_ref_valid(blyt_buffer_h buf, blyt_entity_ref_t ref);

static inline int32_t blyt_buffer_ref_slot(blyt_entity_ref_t ref) {
    return (int32_t)(ref & 0xFFFFu);
}

/* -------------------------------------------------------------------------
 * Save/load (ADR-0087, ADR-0125)
 * ------------------------------------------------------------------------- */
blyt_result_t blyt_save_write(uint32_t slot);
blyt_result_t blyt_save_read(uint32_t slot);

/* -------------------------------------------------------------------------
 * Resources (ADR-0027, ADR-0040, ADR-0088, issues #91/#123)
 *
 * Cart code never uses string paths at runtime: the packer assigns each asset
 * an integer resource id and emits R_<NAME> constants in the generated
 * cart_resources.h.  The runtime resolves id -> bytes internally (a bundled ELF
 * section in a packed cart, or the staging directory in dev mode).
 *
 * Two lifecycle layers (ADR-0027):
 *   - pin/unpin — a within-frame raw-access window.  pin copies the bytes into
 *     a runtime scratch region and hands back a pointer + size; the pointer is
 *     valid for the *current frame only* (the runtime force-releases any pin at
 *     the frame boundary, because dev-mode asset hot-reload may move the bytes
 *     between frames).  Copy out anything you need to keep.  Refcounted: each
 *     pin needs one unpin.
 *   - load/release — residency/caching hints.  load returns a handle (stable
 *     across frames); release tells the runtime the cart is done with it.
 * ------------------------------------------------------------------------- */

/* Packer-assigned integer resource id (the kind-agnostic id used by the
 * lifecycle ECALLs: load/release/pin/unpin). */
typedef uint32_t blyt_resource_id_t;

/* Typed resource-constant aliases (ADR-0068 amendment 2026-06-27, #166). The
 * packer-generated cart_resources.h emits each R_<NAME> as one of these by the
 * resource's kind: a `text` resource is a blyt_text_resource_t, a `raw` one a
 * blyt_bytes_resource_t. Both are uint32_t, so C gives no compile enforcement —
 * the typing documents intent and steers callers to the matching accessor
 * (blyt_resource_text_get vs blyt_resource_bytes_get). The text accessor's
 * trailing-NUL check (below) is the runtime guard against feeding a raw
 * resource to the text path. */
typedef uint32_t blyt_text_resource_t;
typedef uint32_t blyt_bytes_resource_t;

/* Resources are referenced by their baked compile-time constant directly
 * (ADR-0134, #196): there is no cart-held load handle to track or invalidate.
 * The packer emits each R_<NAME> as a console-wide tagged u32 (kind RESOURCE,
 * provenance + 24-bit id); the runtime decodes it, loads-or-cache-hits, and
 * evicts transparently.  The cart just passes the constant to the accessors. */

/* pin: copy the resource's bytes into the runtime scratch region; write the
 * frame-scoped pointer to *out_ptr and the byte length to *out_size, and
 * increment the pin count.  Returns BLYT_OK, or BLYT_ERR_NOT_FOUND for an
 * unknown id (with *out_ptr set to NULL). */
blyt_result_t blyt_resource_pin(blyt_resource_id_t id, const void **out_ptr, size_t *out_size);

/* unpin: drop a pin taken by blyt_resource_pin.  The pointer it returned is
 * invalid afterwards.  Returns BLYT_ERR_INVALID_ARG if nothing was pinned. */
blyt_result_t blyt_resource_unpin(blyt_resource_id_t id);

/* text_get: convenience over pin -> copy -> unpin for *text* resources.  Returns
 * a freshly allocated, NUL-terminated copy of the resource's content bytes (the
 * caller owns it and must free() it) and writes the *content* length to *len —
 * the build-appended trailing NUL is excluded, so *len == strlen (ADR-0088
 * amendment 2026-06-27, #166).  Unlike a pinned pointer this copy outlives the
 * frame.  Returns NULL (leaving *len untouched) on an unknown id, an allocation
 * failure, or a resource whose stored bytes lack the trailing NUL — i.e. a raw
 * resource fed to the text path (this trailing-NUL check is the C error path,
 * since the typedef gives no compile enforcement).  Not its own ECALL — a
 * guest-side helper. */
char *blyt_resource_text_get(blyt_text_resource_t id, size_t *len);

/* bytes_get: the opaque-bytes companion to text_get (#162).  Returns a freshly
 * allocated copy of the resource's *exact* bytes (the caller owns it and must
 * free() it) and writes the byte length to *len.  Unlike text_get it appends no
 * NUL terminator, strips nothing, and makes no text assumption, so it
 * round-trips binary blobs (embedded NULs, high bytes) faithfully — *len is
 * authoritative.  Returns NULL (leaving *len untouched) on an unknown id or
 * allocation failure.  Not its own ECALL — a guest-side helper. */
void *blyt_resource_bytes_get(blyt_bytes_resource_t id, size_t *len);

/* -------------------------------------------------------------------------
 * Memory introspection API (ADR-0029, issue #159)
 *
 * blyt_mem_stats reports cart-visible working-memory usage so a cart can make
 * informed release decisions near the 16 MB budget.
 *
 * DETERMINISM CONTRACT (ADR-0029 amendment, #159) — read before branching on
 * any of these.  The fields are TIERED, and mixing the tiers is a bug:
 *
 *   Deterministic — bit-identical across every peer/platform, SAFE to branch
 *   game logic on:
 *     - budget_cap (always 16 MB)
 *     - the *outcome* of an allocation: a blyt_resource_load / malloc returning
 *       BLYT_RESOURCE_INVALID / NULL at the cap (see ADR-0008 / #158).
 *
 *   Advisory — history-dependent (LRU/eviction order differs across platforms),
 *   MUST NOT feed deterministic game state.  They are a *tuning* signal for
 *   "should I proactively release something", which is itself a memory decision,
 *   not game state:
 *     - resource_cache_used, total_used
 *     - the resources[] residency snapshot (which resources are loaded, and the
 *       loaded count returned)
 * ------------------------------------------------------------------------- */

/* Scalar memory stats.  total_used == cart_allocations + resource_cache_used. */
typedef struct {
    uint32_t resource_cache_used; /* advisory: resident decompressed resource bytes */
    uint32_t cart_allocations; /* live cart heap bytes (guest_heap_used) */
    uint32_t total_used; /* advisory: cart_allocations + resource_cache_used */
    uint32_t budget_cap; /* deterministic: 16 MB working-memory cap */
} blyt_mem_stats_t;

/* One currently-loaded resource and its (decompressed) size, for the
 * enumeration filled by blyt_mem_resources. */
typedef struct {
    blyt_resource_id_t id;
    uint32_t size;
} blyt_mem_resource_t;

/* Fill *out with the current scalar memory stats.  These are published running
 * totals read straight from the guest-visible accounting block (#158) — no host
 * round-trip, no allocation, no traversal (ADR-0029). */
void blyt_mem_stats(blyt_mem_stats_t *out);

/* Enumerate the currently-loaded resources: fill up to `cap` entries of
 * `resources` with their ids and (decompressed) sizes, and return the *total*
 * loaded count — which may exceed `cap` (the array is truncated; the count is
 * not), so a caller can size a follow-up buffer.  Pass `resources == NULL` /
 * `cap == 0` to query just the count.  Unlike blyt_mem_stats this needs the
 * host (the loaded set is host-owned, variable-length table data), so it is the
 * on-demand half of the API — call it only when the per-resource breakdown is
 * actually wanted, not on a hot budget-polling path. */
uint32_t blyt_mem_resources(blyt_mem_resource_t *resources, uint32_t cap);

/* -------------------------------------------------------------------------
 * Cart lifecycle types for save/load callbacks (ADR-0087 amendment)
 * ------------------------------------------------------------------------- */
typedef enum {
    BLYT_LOAD_SAVE_GAME = 0,
    BLYT_LOAD_SAVE_STATE = 1,
    BLYT_LOAD_REWIND = 2,
    BLYT_LOAD_HOT_RELOAD = 3,
} blyt_load_reason_t;

typedef struct {
    bool was_restored; /* false if buffer was entirely absent from save */
    const bool *fields_restored; /* [BLYT_FIELD_INDEX(field_h)] — NULL if !was_restored */
} blyt_buffer_load_info_t;

typedef struct {
    blyt_load_reason_t reason;
    uint32_t saved_cart_version;
    const blyt_buffer_load_info_t *buffers; /* indexed by blyt_buffer_h (1-based) */
} blyt_load_info_t;

/* -------------------------------------------------------------------------
 * Cart lifecycle entry points (ADR-0087)
 *
 * Required — the runtime verifies all three are present at cart load time.
 * The cart defines these; libblyt32.so's blyt_main calls them in order:
 *   init → on_new_state → [update → draw] loop → on_quit → cleanup
 * ------------------------------------------------------------------------- */
void blyt_cart_init(void);
void blyt_cart_update(void);
void blyt_cart_draw(void);

/* Optional — libblyt32.so provides weak no-op defaults for these. */
void blyt_cart_on_new_state(void);
void blyt_cart_on_save_state(void);
void blyt_cart_on_load_state(blyt_load_info_t info);
void blyt_cart_on_quit(void);
void blyt_cart_cleanup(void);

/* Dev-only (issue #122): the runtime calls this after a dev-mode asset hot-swap
 * (`update_assets`, no VM restart) with the changed resource ids, so a cart that
 * derived/cached something from a resource can re-derive only the affected ones.
 * `ids` points to `n` resource handles (blyt_resource_h); order is unspecified.
 * Never fires in a shipped cart.  Weak no-op default in libblytcommon. */
void blyt_cart_on_assets_reloaded(const uint32_t *ids, size_t n);

/* -------------------------------------------------------------------------
 * Cart signals to the runtime (ADR-0087)
 * ------------------------------------------------------------------------- */

/* Signal that the cart is ready to exit the update/draw loop.
 * Call from blyt_cart_on_quit, or directly from blyt_cart_update when the
 * cart decides it is finished (e.g. after showing a credits sequence). */
void blyt_quit(void);

/* Signal the end of one update+draw frame to the host runtime.
 * Called automatically by blyt_main after each blyt_cart_draw(); cart code
 * does not need to call this directly. */
void blyt_frame_done(void);

/* -------------------------------------------------------------------------
 * Debug output (ADR-0085, ECALL 1)
 * ------------------------------------------------------------------------- */
void blyt_console_debug(const char *s);

/* -------------------------------------------------------------------------
 * Graphics — surfaces (Blyt32 variant, ADR-0052/0086/0008; #188 / #195 / #205)
 *
 * A surface is a runtime-managed 256-colour paletted buffer.  BLYT_SCREEN is
 * the built-in 320x240 screen; off-screen surfaces are created with
 * blyt_surface_create.  The tier-1 serviced ops rasterize into a destination
 * surface host-side (reached by ECALL on the emulated path; implemented
 * natively in the native libblyt32 variant).  Colours are palette indices.  A
 * draw into BLYT_SCREEN displaces the boot test card on the first call of a
 * frame; draws into off-screen surfaces reach the screen only via a blit.
 *
 * Surfaces are draw-scoped: created blank inside blyt_cart_draw and auto-reaped
 * at end of draw() (their handles then go stale); they are not saved and count
 * against the 16 MB memory budget.  Handles are the console-wide tagged u32
 * (blyt_handle.h); passing a handle that no longer resolves (wrong kind, stale
 * generation) is a dev error and a defined no-op in release.
 * ------------------------------------------------------------------------- */

/* Console-wide tagged handles (blyt_handle.h).  Aliased to uint32_t so resource
 * constants and surface handles pass to the surface API with no cast. */
typedef uint32_t blyt_surface_h;
typedef uint32_t blyt_lockview_h;

/* The built-in screen surface (SURFACE kind, slot 0, gen 0).  A reserved
 * constant — must equal runtime/shared/blyt_handle.h's BLYT_SCREEN (pinned there
 * by _Static_assert).  Guarded so a guest lib that also includes the canonical
 * blyt_handle.h (native path) gets a single, non-clashing definition. */
#ifndef BLYT_SCREEN
#define BLYT_SCREEN ((blyt_surface_h)0x40000000u)
#endif

/* Create a blank (index 0) off-screen surface of w x h.  Draw-scoped: valid
 * only for the rest of the current draw().  Returns BLYT_HANDLE_NONE (0) on
 * invalid size or when it would exceed the memory budget. */
blyt_surface_h blyt_surface_create(int32_t w, int32_t h);

/* Optionally free a surface early (a no-op on BLYT_SCREEN or a stale handle);
 * otherwise surfaces are reaped automatically at the end of draw(). */
void blyt_surface_destroy(blyt_surface_h surface);

/* Fill the entire destination surface with one palette index. */
void blyt_surface_clear(blyt_surface_h dst, uint8_t color);

/* Set one pixel at (x,y).  Off-surface coordinates are silently clipped
 * (ADR-0048: no clip rect / camera). */
void blyt_surface_pixel(blyt_surface_h dst, int32_t x, int32_t y, uint8_t color);

/* Fill a w x h rectangle with top-left at (x,y); top/left inclusive,
 * bottom/right exclusive.  Clipped to the surface. */
void blyt_surface_rect_fill(blyt_surface_h dst, int32_t x, int32_t y, int32_t w, int32_t h,
                            uint8_t color);

/* Draw a line between (x0,y0) and (x1,y1) inclusive (integer Bresenham). */
void blyt_surface_line(blyt_surface_h dst, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                       uint8_t color);

/* Copy the whole src surface into dst at (x,y), clipped to dst (index copy —
 * the palette is global).  src may be BLYT_SCREEN or a created surface. */
void blyt_surface_blit(blyt_surface_h dst, blyt_surface_h src, int32_t x, int32_t y);

/* -------------------------------------------------------------------------
 * Tier-2 per-pixel surface lock (#205)
 *
 * For raw per-pixel work, acquire a surface: the runtime materializes its
 * buffer where the cart can read/write it and hands back a blyt_lock_t.  Write
 * pixels directly through lock.pixels (row-major, lock.stride bytes per row) or
 * draw with the freestanding blyt_raster_* primitives below (guest-side, no
 * ECALL) — the SAME rasterizer the tier-1 ops use, so lock drawing is
 * pixel-identical.  Release when done: the buffer is flushed back and the lock
 * token goes stale.  The lock is exclusive per surface and valid only until
 * release or the end of draw(); the pointer must not be used after release.
 * ------------------------------------------------------------------------- */
typedef struct {
    uint8_t *pixels; /* materialized buffer (row-major palette indices) */
    int32_t stride; /* bytes per row (== w) */
    int32_t w, h; /* surface dimensions */
    blyt_lockview_h token; /* pass to blyt_surface_release; BLYT_HANDLE_NONE on failure */
} blyt_lock_t;

/* Acquire an exclusive per-pixel lock on `surface`.  Fills *out and returns 1 on
 * success; on failure (unresolvable surface, already locked) zeroes *out (token
 * BLYT_HANDLE_NONE) and returns 0. */
int32_t blyt_surface_acquire(blyt_surface_h surface, blyt_lock_t *out);

/* Release a lock: flush the materialized buffer back to the surface and
 * invalidate the token.  A no-op on a stale/foreign token. */
void blyt_surface_release(blyt_lock_t *lock);

/* In-lock drawing primitives — the freestanding integer rasterizer
 * (runtime/shared/blyt_raster.h), called on a locked surface's buffer.  Bounds
 * are the lock's (stride, w, h).  Identical source to the tier-1 host handlers. */
void blyt_raster_clear(uint8_t *fb, int stride, int width, int height, uint8_t color);
void blyt_raster_pixel(uint8_t *fb, int stride, int width, int height, int x, int y, uint8_t color);
void blyt_raster_rect_fill(uint8_t *fb, int stride, int width, int height, int x, int y, int w,
                           int h, uint8_t color);
void blyt_raster_line(uint8_t *fb, int stride, int width, int height, int x0, int y0, int x1,
                      int y1, uint8_t color);
void blyt_raster_blit(uint8_t *dst, int dstride, int dwidth, int dheight, const uint8_t *src,
                      int sstride, int swidth, int sheight, int x, int y);

/* gfx.* — literal sugar over BLYT_SCREEN (ADR-0046 canvas-is-receiver, #205).
 * The screen-targeting shorthand; not a parallel path — the same host-side
 * rasterizer, so they cannot drift from the surface API. */
static inline void blyt_gfx_clear(uint8_t color) {
    blyt_surface_clear(BLYT_SCREEN, color);
}
static inline void blyt_gfx_pixel(int32_t x, int32_t y, uint8_t color) {
    blyt_surface_pixel(BLYT_SCREEN, x, y, color);
}
static inline void blyt_gfx_rect_fill(int32_t x, int32_t y, int32_t w, int32_t h, uint8_t color) {
    blyt_surface_rect_fill(BLYT_SCREEN, x, y, w, h, color);
}
static inline void blyt_gfx_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint8_t color) {
    blyt_surface_line(BLYT_SCREEN, x0, y0, x1, y1, color);
}

/* Direct framebuffer access (issue #188 / Spike X, Q1) — the fixed-region raw
 * path; superseded by the tier-2 surface lock (blyt_surface_acquire, #205) but
 * kept until that lands.  blyt_gfx_acquire() returns a pointer to the 320x240
 * paletted back buffer; blyt_gfx_present() flushes those writes and displaces
 * the boot test card. */
uint8_t *blyt_gfx_acquire(void);
void blyt_gfx_present(void);

/* -------------------------------------------------------------------------
 * Graphics — palette (ADR-0042/0086, issue #201)
 *
 * The screen's 256-entry palette (XRGB8888) is global.  blyt_gfx_palette_set
 * loads one of the four runtime-bundled built-in palettes wholesale.  A cart
 * that declares no palette (`palettes:` in blyt.config.yaml) gets
 * BLYT_PALETTE_DEFAULT (aurora) auto-loaded before init() (ADR-0088).  There
 * is no cart-authored palette yet -- custom palettes are #203.
 *
 * Handles are console-wide tagged resource constants (blyt_handle.h) with
 * runtime provenance -- must equal runtime/shared/blyt_palettes.h's builtin ids
 * encoded via BLYT_RESOURCE_ENCODE(id, BLYT_RESOURCE_PROV_RUNTIME).  Guarded so
 * a guest lib that also includes the canonical blyt_handle.h (native path)
 * gets a single, non-clashing definition, matching the BLYT_SCREEN pattern
 * above. */
#ifndef BLYT_PALETTE_AURORA
#define BLYT_PALETTE_AURORA ((blyt_resource_id_t)0x21000001u)
#endif
#ifndef BLYT_PALETTE_VGA
#define BLYT_PALETTE_VGA ((blyt_resource_id_t)0x21000002u)
#endif
#ifndef BLYT_PALETTE_EGA
#define BLYT_PALETTE_EGA ((blyt_resource_id_t)0x21000003u)
#endif
#ifndef BLYT_PALETTE_CGA
#define BLYT_PALETTE_CGA ((blyt_resource_id_t)0x21000004u)
#endif
/* The console default -- what an undeclared-palette cart auto-loads. */
#ifndef BLYT_PALETTE_DEFAULT
#define BLYT_PALETTE_DEFAULT BLYT_PALETTE_AURORA
#endif

/* Load a built-in palette wholesale (all 256 entries).  A no-op on a handle
 * that does not resolve to a built-in palette. */
void blyt_gfx_palette_set(blyt_resource_id_t palette);

/* -------------------------------------------------------------------------
 * Graphics — named color-index constants (ADR-0059, issue #203)
 *
 * Plain compile-time palette *indices* (0-255), NOT the tagged resource
 * handles that BLYT_PALETTE_* above are.  They let a cart draw text / basic
 * graphics in known colors without defining indices first.  The EGA-16 is the
 * shared naming vocabulary; each bundled palette ships its own set matching
 * that palette's layout, so the same name is a different index per palette:
 *
 *   BLYT_EGA_*    canonical EGA indices 0-15.
 *   BLYT_VGA_*    alias BLYT_EGA_* -- vga's low 16 ARE the EGA 16.
 *   BLYT_AURORA_* the nearest-to-EGA index in the Aurora 256 (grays + black/
 *                 white exact, chromatics hue-shifted; distance table in #203).
 *
 * A cart uses the set matching its active palette; there is no runtime
 * resolution and nothing is deterministic-sensitive (these are literals).  The
 * unprefixed BLYT_<NAME> aliases the console-default palette (aurora) for
 * zero-config text -- but stops being semantically correct after a palette
 * swap, so prefer the palette-specific set once the palette is known.  Custom
 * (cart) palettes get packer-generated C_<NAME> constants instead (ADR-0059).
 *
 * EGA quirk: index 6 is BROWN (#AA5500), not a dark yellow, and there is no
 * bright-brown -- the bright row is the BR_* names. */

/* EGA-16 canonical indices 0-15. */
#define BLYT_EGA_BLACK 0
#define BLYT_EGA_BLUE 1
#define BLYT_EGA_GREEN 2
#define BLYT_EGA_CYAN 3
#define BLYT_EGA_RED 4
#define BLYT_EGA_MAGENTA 5
#define BLYT_EGA_BROWN 6
#define BLYT_EGA_LTGRAY 7
#define BLYT_EGA_DKGRAY 8
#define BLYT_EGA_BR_BLUE 9
#define BLYT_EGA_BR_GREEN 10
#define BLYT_EGA_BR_CYAN 11
#define BLYT_EGA_BR_RED 12
#define BLYT_EGA_BR_MAGENTA 13
#define BLYT_EGA_BR_YELLOW 14
#define BLYT_EGA_WHITE 15

/* VGA's low 16 are the EGA 16 (identical RGB); alias them. */
#define BLYT_VGA_BLACK BLYT_EGA_BLACK
#define BLYT_VGA_BLUE BLYT_EGA_BLUE
#define BLYT_VGA_GREEN BLYT_EGA_GREEN
#define BLYT_VGA_CYAN BLYT_EGA_CYAN
#define BLYT_VGA_RED BLYT_EGA_RED
#define BLYT_VGA_MAGENTA BLYT_EGA_MAGENTA
#define BLYT_VGA_BROWN BLYT_EGA_BROWN
#define BLYT_VGA_LTGRAY BLYT_EGA_LTGRAY
#define BLYT_VGA_DKGRAY BLYT_EGA_DKGRAY
#define BLYT_VGA_BR_BLUE BLYT_EGA_BR_BLUE
#define BLYT_VGA_BR_GREEN BLYT_EGA_BR_GREEN
#define BLYT_VGA_BR_CYAN BLYT_EGA_BR_CYAN
#define BLYT_VGA_BR_RED BLYT_EGA_BR_RED
#define BLYT_VGA_BR_MAGENTA BLYT_EGA_BR_MAGENTA
#define BLYT_VGA_BR_YELLOW BLYT_EGA_BR_YELLOW
#define BLYT_VGA_WHITE BLYT_EGA_WHITE

/* Aurora nearest-to-EGA indices (issue #203 distance table). */
#define BLYT_AURORA_BLACK 0
#define BLYT_AURORA_BLUE 223
#define BLYT_AURORA_GREEN 185
#define BLYT_AURORA_CYAN 195
#define BLYT_AURORA_RED 155
#define BLYT_AURORA_MAGENTA 239
#define BLYT_AURORA_BROWN 165
#define BLYT_AURORA_LTGRAY 10
#define BLYT_AURORA_DKGRAY 5
#define BLYT_AURORA_BR_BLUE 219
#define BLYT_AURORA_BR_GREEN 189
#define BLYT_AURORA_BR_CYAN 201
#define BLYT_AURORA_BR_RED 160
#define BLYT_AURORA_BR_MAGENTA 236
#define BLYT_AURORA_BR_YELLOW 175
#define BLYT_AURORA_WHITE 15

/* Unprefixed default aliases -> the console-default palette (aurora). */
#define BLYT_BLACK BLYT_AURORA_BLACK
#define BLYT_BLUE BLYT_AURORA_BLUE
#define BLYT_GREEN BLYT_AURORA_GREEN
#define BLYT_CYAN BLYT_AURORA_CYAN
#define BLYT_RED BLYT_AURORA_RED
#define BLYT_MAGENTA BLYT_AURORA_MAGENTA
#define BLYT_BROWN BLYT_AURORA_BROWN
#define BLYT_LTGRAY BLYT_AURORA_LTGRAY
#define BLYT_DKGRAY BLYT_AURORA_DKGRAY
#define BLYT_BR_BLUE BLYT_AURORA_BR_BLUE
#define BLYT_BR_GREEN BLYT_AURORA_BR_GREEN
#define BLYT_BR_CYAN BLYT_AURORA_BR_CYAN
#define BLYT_BR_RED BLYT_AURORA_BR_RED
#define BLYT_BR_MAGENTA BLYT_AURORA_BR_MAGENTA
#define BLYT_BR_YELLOW BLYT_AURORA_BR_YELLOW
#define BLYT_WHITE BLYT_AURORA_WHITE

/* -------------------------------------------------------------------------
 * Lua export macros (ADR-0111) — hybrid Lua+C carts
 *
 * Usage:
 *   BLYT_LUA_EXPORT_VOID(my_fn) { ... }
 *   BLYT_LUA_EXPORT_I32(my_fn, int32_t x) { return x + 1; }
 *   BLYT_LUA_MODULE_EXPORT_VOID(mylib, my_fn) { ... }
 *   BLYT_LUA_MODULE_EXPORT_I32(mylib, my_fn, int32_t x) { return x + 1; }
 *
 * No pre-requisite includes needed: blyt.h pulls in the minimal Lua API
 * declarations automatically.  Cart code must NOT include lua.h directly.
 *
 * Each macro:
 *   1. Emits a Lua C wrapper (__lua_export_NAME) callable by the Lua VM.
 *   2. Emits a blyt_lua_regtab_entry_t in .lua_regtab, iterated by the
 *      SDK-generated cart_lua_modules on SDL2/libretro/WASM-native.
 *   3. Emits a blyt_lua_export_entry_t in .lua_exports, parsed by the WASM
 *      host to build trampolines that call the function via rv32emu.
 *   4. Defines the underlying C function (user writes the body).
 *
 * BLYT_LUA_MODULE_EXPORT_* registers the function in a Lua module table so
 * Lua code can call it via require("mylib").my_fn(x).  The underlying C
 * symbol is named mylib_my_fn; the Lua name is "mylib.my_fn".  Use this
 * instead of cart_lua_modules / luaopen_* which are not portable to WASM.
 * ------------------------------------------------------------------------- */

/* Pull in minimal Lua type and API declarations if lua.h not already loaded. */
#ifndef LUA_VERSION_NUM
#include "blyt_lua_internal.h"
#endif

/* Type codes for BLYT_LUA_EXPORT primitive types. */
#define BLYT_LUA_TYPE_VOID 0
#define BLYT_LUA_TYPE_I32 1
#define BLYT_LUA_TYPE_U32 2
#define BLYT_LUA_TYPE_F32 3
#define BLYT_LUA_TYPE_BOOL 4

/* Export entry flags (ADR-0130).  BRIDGED: the WASM host invokes the wrapper
 * (wrap_sym) through the ECALL-bridged Lua C API instead of typed register
 * conversion — enables strings, tables, >4 args, and multiple returns. */
#define BLYT_LUA_EXPORT_FLAG_BRIDGED 0x01

/* One entry per exported function, placed in .lua_exports by the macros below.
 * The host reads this section to resolve guest addresses without raw pointers
 * (which would require relocation processing).
 * For module exports lua_name uses dotted notation: "module.fn". */
typedef struct {
    char lua_name[32]; /* Lua name: "fn" for globals, "mod.fn" for modules */
    char fn_sym[64]; /* underlying C function symbol name */
    char wrap_sym[64]; /* Lua C wrapper symbol: __lua_export_<fn_sym> */
    uint8_t nargs;
    uint8_t arg_types[4]; /* BLYT_LUA_TYPE_* for each arg */
    uint8_t ret_type; /* BLYT_LUA_TYPE_* */
    uint8_t flags; /* BLYT_LUA_EXPORT_FLAG_* (was _pad[0]; 0 in old carts) */
    uint8_t _pad;
} blyt_lua_export_entry_t; /* 168 bytes */

/* One entry per exported function, placed in .lua_regtab by the macros below.
 * Iterated by the SDK-generated cart_lua_modules to register exports with the
 * Lua VM.  module_name == NULL means a global export; non-NULL registers the
 * function into a module table accessible via require(module_name). */
typedef struct {
    const char *module_name; /* NULL = global export */
    const char *lua_fn_name; /* fn name within module, or global name */
    lua_CFunction wrapper; /* the __lua_export_* CFunction */
} blyt_lua_regtab_entry_t;

/* clang-format off */

/* 0 args, void return, global export */
#define BLYT_LUA_EXPORT_VOID(name) \
    void name(void); \
    int __lua_export_##name(lua_State *L) { \
        (void)L; name(); return 0; \
    } \
    static const blyt_lua_regtab_entry_t __lua_regtab_##name \
        __attribute__((used, retain, section(".lua_regtab"))) = { \
        0, #name, __lua_export_##name \
    }; \
    static const blyt_lua_export_entry_t __export_##name \
        __attribute__((used, retain, section(".lua_exports"))) = { \
        #name, #name, "__lua_export_" #name, \
        0, {0, 0, 0, 0}, BLYT_LUA_TYPE_VOID, 0, 0 \
    }; \
    void name(void)

/* 1 I32 arg, I32 return, global export.
 * The variadic arg is the C parameter declaration (e.g. "int32_t x"). */
#define BLYT_LUA_EXPORT_I32(name, ...) \
    int32_t name(__VA_ARGS__); \
    int __lua_export_##name(lua_State *L) { \
        int32_t _a0 = (int32_t)lua_tointeger(L, 1); \
        int32_t _r = name(_a0); \
        lua_pushinteger(L, (lua_Integer)_r); \
        return 1; \
    } \
    static const blyt_lua_regtab_entry_t __lua_regtab_##name \
        __attribute__((used, retain, section(".lua_regtab"))) = { \
        0, #name, __lua_export_##name \
    }; \
    static const blyt_lua_export_entry_t __export_##name \
        __attribute__((used, retain, section(".lua_exports"))) = { \
        #name, #name, "__lua_export_" #name, \
        1, {BLYT_LUA_TYPE_I32, 0, 0, 0}, BLYT_LUA_TYPE_I32, 0, 0 \
    }; \
    int32_t name(__VA_ARGS__)

/* 0 args, void return, module export.
 * The underlying C function is named module##_##fn_name; the Lua name is
 * "module.fn_name" accessible via require("module").fn_name(). */
#define BLYT_LUA_MODULE_EXPORT_VOID(module, fn_name) \
    void module##_##fn_name(void); \
    int __lua_export_##module##_##fn_name(lua_State *L) { \
        (void)L; module##_##fn_name(); return 0; \
    } \
    static const blyt_lua_regtab_entry_t __lua_regtab_##module##_##fn_name \
        __attribute__((used, retain, section(".lua_regtab"))) = { \
        #module, #fn_name, __lua_export_##module##_##fn_name \
    }; \
    static const blyt_lua_export_entry_t __export_##module##_##fn_name \
        __attribute__((used, retain, section(".lua_exports"))) = { \
        #module "." #fn_name, #module "_" #fn_name, \
        "__lua_export_" #module "_" #fn_name, \
        0, {0, 0, 0, 0}, BLYT_LUA_TYPE_VOID, 0, 0 \
    }; \
    void module##_##fn_name(void)

/* 1 I32 arg, I32 return, module export. */
#define BLYT_LUA_MODULE_EXPORT_I32(module, fn_name, ...) \
    int32_t module##_##fn_name(__VA_ARGS__); \
    int __lua_export_##module##_##fn_name(lua_State *L) { \
        int32_t _a0 = (int32_t)lua_tointeger(L, 1); \
        int32_t _r = module##_##fn_name(_a0); \
        lua_pushinteger(L, (lua_Integer)_r); \
        return 1; \
    } \
    static const blyt_lua_regtab_entry_t __lua_regtab_##module##_##fn_name \
        __attribute__((used, retain, section(".lua_regtab"))) = { \
        #module, #fn_name, __lua_export_##module##_##fn_name \
    }; \
    static const blyt_lua_export_entry_t __export_##module##_##fn_name \
        __attribute__((used, retain, section(".lua_exports"))) = { \
        #module "." #fn_name, #module "_" #fn_name, \
        "__lua_export_" #module "_" #fn_name, \
        1, {BLYT_LUA_TYPE_I32, 0, 0, 0}, BLYT_LUA_TYPE_I32, 0, 0 \
    }; \
    int32_t module##_##fn_name(__VA_ARGS__)

/* Raw exports (ADR-0130): the author writes the Lua C wrapper body directly
 * against the restricted Lua C API (blyt_lua_internal.h) — strings, tables,
 * any number of arguments, multiple returns, luaL_error.  The body has the
 * shape of a lua_CFunction: read args from the stack, push results, return
 * the result count.  The same body runs on every target: real Lua C API on
 * rv32; ECALL-bridged on WASM (flags = BLYT_LUA_EXPORT_FLAG_BRIDGED).
 *
 * Usage:
 *   BLYT_LUA_EXPORT_RAW(greet) {
 *       const char *who = luaL_checkstring(L, 1);
 *       ...
 *       lua_pushstring(L, out);
 *       return 1;
 *   }
 */
#define BLYT_LUA_EXPORT_RAW(name) \
    int __blyt_lua_raw_##name(lua_State *L); \
    static const blyt_lua_regtab_entry_t __lua_regtab_##name \
        __attribute__((used, retain, section(".lua_regtab"))) = { \
        0, #name, __blyt_lua_raw_##name \
    }; \
    static const blyt_lua_export_entry_t __export_##name \
        __attribute__((used, retain, section(".lua_exports"))) = { \
        #name, "__blyt_lua_raw_" #name, "__blyt_lua_raw_" #name, \
        0, {0, 0, 0, 0}, BLYT_LUA_TYPE_VOID, BLYT_LUA_EXPORT_FLAG_BRIDGED, 0 \
    }; \
    int __blyt_lua_raw_##name(lua_State *L)

#define BLYT_LUA_MODULE_EXPORT_RAW(module, fn_name) \
    int __blyt_lua_raw_##module##_##fn_name(lua_State *L); \
    static const blyt_lua_regtab_entry_t __lua_regtab_##module##_##fn_name \
        __attribute__((used, retain, section(".lua_regtab"))) = { \
        #module, #fn_name, __blyt_lua_raw_##module##_##fn_name \
    }; \
    static const blyt_lua_export_entry_t __export_##module##_##fn_name \
        __attribute__((used, retain, section(".lua_exports"))) = { \
        #module "." #fn_name, \
        "__blyt_lua_raw_" #module "_" #fn_name, \
        "__blyt_lua_raw_" #module "_" #fn_name, \
        0, {0, 0, 0, 0}, BLYT_LUA_TYPE_VOID, BLYT_LUA_EXPORT_FLAG_BRIDGED, 0 \
    }; \
    int __blyt_lua_raw_##module##_##fn_name(lua_State *L)

/* clang-format on */

#ifdef __cplusplus
}
#endif
