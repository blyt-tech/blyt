/*
 * libblytcommon — variant-agnostic blyt API, native RISC-V path (issue #128).
 *
 * The real-work implementations of the variant-agnostic APIs (state buffers,
 * save/load, frame-boundary FP determinism, console debug, startup) for the
 * native bare-metal target.  On the emulated path these are ECALL stubs the
 * host backs; here the guest library *is* the whole runtime, so they use real
 * Linux syscalls and operate on in-process storage.  Built into the native
 * libblytcommon.so variant and installed in sdk/lib/native/ (recompiled, not
 * copied from the emulated variant).
 *
 * Determinism-critical primitives shared with the host runtime — NaN
 * canonicalization (ADR-0010), the generation-counter wrap (ADR-0096), and the
 * cart ELF section locator — come from runtime/shared so there is exactly one
 * definition host==native.  The portable lifecycle driver (blyt_main) is the
 * shared runtime/guest/src/libblytcommon/blyt_common.c, compiled into this
 * variant alongside this file.  blyt_exit (variant-specific process exit) stays
 * in the native libblyt32 variant.
 *
 * C library (malloc, getenv, etc.) comes from ld-blyt.so.1 (system musl) via
 * the DT_NEEDED chain: libblyt32.so → libblytcommon.so → libblytc.so →
 * ld-blyt.so.1.
 */

#include <stddef.h>
#include <stdint.h>

#include "blyt.h"
#include "blyt_blys.h" /* runtime/shared: shared BLYS save format (ADR-0125, #129) */
#include "blyt_elf_section.h" /* runtime/shared: blyt_elf32_find_section */
#include "blyt_fp_canon.h" /* runtime/shared: blyt_canon_f32/f64 (ADR-0010) */
#include "blyt_gen.h" /* runtime/shared: blyt_gen_next (ADR-0096) */
#include "blyt_native_trace.h"
#include "blyt_resource_lifecycle.h" /* runtime/shared: load/pin refcount state (#123) */
#include "seccomp_restricted.h"

/* getenv is declared in blyt_native_trace.h (provided by ld-blyt.so.1). */

/* ── Linux ABI constants (inline — no linux/fcntl.h dependency) ─────────── */

#define NATIVE_AT_FDCWD (-100)
#define NATIVE_O_RDONLY 0
#define NATIVE_O_WRONLY 1
#define NATIVE_O_CREAT 64 /* 0100 octal */
#define NATIVE_O_TRUNC 512 /* 01000 octal */

/* ── Minimal string helpers (no external deps) ──────────────────────────── */

static unsigned int blyt32_native_strlen(const char *s) {
    const char *p = s;
    while (*p)
        p++;
    return (unsigned int)(p - s);
}

static void blyt32_native_memcpy(void *d, const void *s, unsigned int n) {
    __builtin_memcpy(d, s, n);
}

static void blyt32_native_memset(void *d, int c, unsigned int n) {
    __builtin_memset(d, c, n);
}

/* Build a save path: <save_dir>/slot_<slot_num>.blys (no snprintf needed). */
static unsigned int build_save_path(char *dst, unsigned int cap, const char *save_dir,
                                    uint32_t slot_num) {
    unsigned int i = 0;
    /* copy save_dir */
    while (save_dir[i] && i + 1 < cap) {
        dst[i] = save_dir[i];
        i++;
    }
    /* append "/slot_" */
    const char pfx[] = "/slot_";
    for (unsigned int j = 0; pfx[j] && i + 1 < cap; j++)
        dst[i++] = pfx[j];
    /* append decimal slot number */
    char num[12];
    int ni = 0;
    uint32_t n = slot_num;
    do {
        num[ni++] = (char)('0' + (int)(n % 10u));
        n /= 10u;
    } while (n);
    for (int j = ni - 1; j >= 0 && i + 1 < cap; j--)
        dst[i++] = num[j];
    /* append ".blys" */
    const char sfx[] = ".blys";
    for (unsigned int j = 0; sfx[j] && i + 1 < cap; j++)
        dst[i++] = sfx[j];
    if (i < cap)
        dst[i] = '\0';
    return i;
}

/* ── Raw I/O helpers (loop until all bytes transferred) ─────────────────── */

static int write_all(int fd, const void *buf, unsigned int len) {
    const char *p = (const char *)buf;
    while (len > 0) {
        unsigned int chunk = len > 65536u ? 65536u : len;
        register long a0 __asm__("a0") = fd;
        register const char *a1 __asm__("a1") = p;
        register long a2 __asm__("a2") = (long)chunk;
        register long a7 __asm__("a7") = 64; /* SYS_write */
        __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
        if (a0 <= 0)
            return -1;
        p += (long)a0;
        len -= (unsigned int)(long)a0;
    }
    return 0;
}

static int read_all(int fd, void *buf, unsigned int len) {
    char *p = (char *)buf;
    unsigned int got = 0;
    while (got < len) {
        unsigned int chunk = (len - got) > 65536u ? 65536u : (len - got);
        register long a0 __asm__("a0") = fd;
        register void *a1 __asm__("a1") = (void *)p;
        register long a2 __asm__("a2") = (long)chunk;
        register long a7 __asm__("a7") = 63; /* SYS_read */
        __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
        if (a0 <= 0)
            return (a0 == 0) ? 1 : -1; /* 1 = unexpected EOF */
        p += (long)a0;
        got += (unsigned int)(long)a0;
    }
    return 0;
}

/* ── Native state buffer storage (field-major true-width arena, #134) ─────── */

/* Fixed-dimension limits.  Field handles encode (buf_id-1, field_index-1);
 * slot indices are zero-based.  These must be ≥ the cart's declared values. */
#define NATIVE_MAX_BUF 8
/* Must equal BLYS_MAX_SLOTS (host BLYT_MAX_SLOTS) so the on-disk slot bitset and
 * generation blobs are byte-identical across platforms (#129). */
#define NATIVE_MAX_SLOTS BLYS_MAX_SLOTS
#define NATIVE_MAX_FIELDS 64

/* type_tag encoding (schemas/cart_layouts.fbs; matches host state_buffer.c). */
#define NATIVE_TYPE_I8 0
#define NATIVE_TYPE_U8 1
#define NATIVE_TYPE_I16 2
#define NATIVE_TYPE_U16 3
#define NATIVE_TYPE_I32 4
#define NATIVE_TYPE_U32 5
#define NATIVE_TYPE_F32 6
#define NATIVE_TYPE_BOOL 7
#define NATIVE_TYPE_F64 8 /* Spike U: 64-bit double field */

/* True width (bytes) of a field's stored value, by type_tag.  Matches the
 * host's field_sizeof (state_buffer.c) so the on-wire per-field arrays are
 * byte-identical in layout. */
static uint32_t field_width(uint8_t type_tag) {
    switch (type_tag) {
    case NATIVE_TYPE_I8:
    case NATIVE_TYPE_U8:
    case NATIVE_TYPE_BOOL:
        return 1u;
    case NATIVE_TYPE_I16:
    case NATIVE_TYPE_U16:
        return 2u;
    case NATIVE_TYPE_F64:
        return 8u;
    case NATIVE_TYPE_I32:
    case NATIVE_TYPE_U32:
    case NATIVE_TYPE_F32:
    default:
        return 4u;
    }
}

/* Field-major true-width storage (issue #134).  A per-buffer byte arena with a
 * fixed per-field stride; within a field, slots are packed at the field's true
 * width (element s at offset s*width).  This matches the host's per-field
 * contiguous layout (blyt_buffer_ctx_t.field_data[fi] in state_buffer.c), so
 * the BLYS save body is a plain per-field memcpy with no transpose/narrow, and
 * the shared BLYS serializer (#129) needs no native-specific byte path.
 *
 * Worst-case footprint = NATIVE_MAX_BUF * NATIVE_MAX_FIELDS * NATIVE_MAX_SLOTS
 * * 8 = 256 KiB — identical to the previous widened uint64 cube.  Still BSS /
 * zero-init / heap-free (no .init_array constructors run on this path, #43):
 * empty slots and zero values come free from the BSS zero-fill.  An 8-aligned
 * base plus an 8-multiple field stride makes every field sub-array (including
 * f64) naturally 8-aligned. */
#define NATIVE_FIELD_STRIDE (NATIVE_MAX_SLOTS * 8u)
#define NATIVE_BUF_STRIDE (NATIVE_MAX_FIELDS * NATIVE_FIELD_STRIDE)
static uint8_t s_arena[NATIVE_MAX_BUF][NATIVE_BUF_STRIDE] __attribute__((aligned(8)));

/* Base of buffer bi's field fi within the arena (element s at + s*width). */
static uint8_t *field_base(uint32_t bi, uint32_t fi) {
    return &s_arena[bi][fi * NATIVE_FIELD_STRIDE];
}

/* Per-buffer field type tags and field counts, flattened from .cart.layouts at
 * startup (load_cart_buf_counts).  Used by the per-field save/load memcpy and
 * the free_slot zeroing loop to know each field's true width; the typed
 * accessors use their own intrinsic width.  Zero (BSS) until parsed: a buffer
 * with s_field_count==0 contributes no save bytes and zeroes no fields. */
static uint8_t s_field_type[NATIVE_MAX_BUF][NATIVE_MAX_FIELDS];
static uint32_t s_field_count[NATIVE_MAX_BUF];

/* Slot allocation bitset: bit i of byte (i/8) indicates slot i is allocated. */
static uint8_t s_slot_bits[NATIVE_MAX_BUF][NATIVE_MAX_SLOTS / 8];

/* Per-slot generation counters (ADR-0096), stored UNBIASED (1..65535; 0 is
 * reserved so a packed ref to slot 0 never equals BLYT_ENTITY_REF_NONE).
 * Initialised to 1 at startup (load_cart_buf_counts) — ELF constructors do not
 * run on this path (#43), so the init is explicit — matching the host
 * (state_buffer.c).  Bumped on successful free_slot via the shared blyt_gen_next
 * primitive.  Stored unbiased so s_gen is the exact 256-byte on-disk gens blob
 * the shared BLYS serializer writes, byte-identical to the host (#129). */
static uint16_t s_gen[NATIVE_MAX_BUF][NATIVE_MAX_SLOTS];

/* Per-buffer declared slot count from .cart.layouts, loaded at startup.
 * Initialised to NATIVE_MAX_SLOTS in load_cart_buf_counts(); until then the
 * array is zero (BSS) but slot functions are not called before startup. */
static uint32_t s_buf_count[NATIVE_MAX_BUF];

/* Number of declared state buffers (BufferDecl count from .cart.layouts, capped
 * at NATIVE_MAX_BUF); set in load_cart_buf_counts().  Drives how many CART
 * sections the BLYS save emits, in declaration order (matches the host). */
static uint32_t s_n_buffers;

/* Per-buffer name (BufferDecl.name), copied out of the cart ELF at startup
 * (the mmap is released before save time).  Emitted in each CART section so the
 * save is self-describing and matches the host byte-for-byte (#129). */
#define NATIVE_BUF_NAME_CAP 64
static char s_buf_name[NATIVE_MAX_BUF][NATIVE_BUF_NAME_CAP];
static uint32_t s_buf_name_len[NATIVE_MAX_BUF];

/* The cart's manifest schema_hash (CartLayouts.schema_hash), loaded at startup.
 * Stamped into the BLYS header on write and checked against on read (#129). */
static uint64_t s_schema_hash;

/* The running cart's .cart.config save_version (ADR-0125), loaded at startup
 * by load_cart_buf_counts().  Stamped into the save header on write and
 * reported as blyt_load_info_t.saved_cart_version on read.  0 if undeclared. */
static uint32_t s_save_version;

/* Per-slot public generation (unbiased; see s_gen). */
static uint16_t native_gen(uint32_t bi, int32_t s) {
    return s_gen[bi][s];
}

static int slot_allocated(uint32_t bi, int32_t slot) {
    return (s_slot_bits[bi][(uint32_t)slot / 8u] >> ((uint32_t)slot % 8u)) & 1u;
}

/* Validate a (buffer, slot, field) reference — all values are zero-based.
 * The slot must be allocated: the emulated path (state_buffer.c) rejects
 * get/set on unallocated slots, so the native path must too. */
static int ref_ok(uint32_t bi, int32_t slot, uint32_t fi) {
    return bi < NATIVE_MAX_BUF && slot >= 0 && (uint32_t)slot < s_buf_count[bi] &&
           fi < NATIVE_MAX_FIELDS && slot_allocated(bi, slot);
}

/* ── Startup initialisation (called from _blyt_entry) ───────────────────
 *
 * Called from _blyt_entry (generated by devtool) before blyt_main.
 * musl ILP32 ld.so does not invoke .init_array constructors on this
 * custom entry path (issue #43); called explicitly here instead.
 */

/* Parse the cart ELF's .cart.layouts section to load per-buffer declared slot
 * counts into s_buf_count[] and per-field type tags into s_field_type[]/
 * s_field_count[] (#134).  Must be called before blyt_install_restricted_filter():
 * uses openat/statx/mmap/munmap which are in the launcher's RV32 filter but not
 * in the restricted filter.  On any error the NATIVE_MAX_SLOTS defaults stand
 * (and that buffer's field count stays 0).
 *
 * The ELF section walk is shared with the host (runtime/shared); the inline
 * FlatBuffer reads (below, via the fb_* helpers) stay native-side (host uses flatcc).
 *   CartLayouts: field 0 (records) at vtable offset 4, field 1 (buffers) at offset 6
 *   BufferDecl:  field 1 (record_name) at offset 6, field 2 (count) at offset 8
 *   RecordDecl:  field 0 (name) at offset 4, field 1 (fields) at offset 6
 *   FieldDecl:   field 1 (type_tag) at offset 6
 *   CartConfig:  field 1 (save_version) at vtable offset 6
 */
/* ── Minimal inline FlatBuffer readers (bounds-checked against [fb,fb+size)) ──
 * Used to walk records[]/fields[] for the per-field type tags (#134); the host
 * uses flatcc, this path stays dependency-free.  All return 0/NULL on any
 * out-of-range or absent field. */

/* u16 offset of `table`'s field at vtable byte offset `voff`; 0 if absent.
 * The soffset is signed: a vtable may sit before OR after its table (flatcc
 * deduplicates vtables, so inner tables like FieldDecl can reference a vtable
 * emitted after them — a negative soffset).  Handle both directions and bound
 * the resolved vtable against the buffer rather than assuming soffset > 0. */
static uint16_t fb_field_off(const uint8_t *fb, uint32_t fb_size, const uint8_t *table,
                             uint16_t voff) {
    int32_t soff;
    blyt32_native_memcpy(&soff, table, 4);
    if (soff == 0)
        return 0;
    const uint8_t *vtable = table - soff; /* soff signed; subtract per spec */
    if (vtable < fb || vtable + 4u > fb + fb_size)
        return 0;
    uint16_t vtsize;
    blyt32_native_memcpy(&vtsize, vtable, 2);
    if (vtsize < (uint32_t)voff + 2u || vtable + vtsize > fb + fb_size)
        return 0;
    uint16_t foff;
    blyt32_native_memcpy(&foff, vtable + voff, 2);
    return foff;
}

/* Absolute address of a uoffset field (sub-table / vector / string); NULL if absent. */
static const uint8_t *fb_uoffset_field(const uint8_t *fb, uint32_t fb_size, const uint8_t *table,
                                       uint16_t voff) {
    uint16_t foff = fb_field_off(fb, fb_size, table, voff);
    if (foff == 0)
        return NULL;
    const uint8_t *fptr = table + foff;
    if (fptr + 4u > fb + fb_size)
        return NULL;
    uint32_t uoff;
    blyt32_native_memcpy(&uoff, fptr, 4);
    const uint8_t *target = fptr + uoff;
    if (target < fb || target + 4u > fb + fb_size)
        return NULL;
    return target;
}

/* Element i of a uoffset vector `vec` (len at vec[0]); NULL if out of range. */
static const uint8_t *fb_vec_at(const uint8_t *fb, uint32_t fb_size, const uint8_t *vec,
                                uint32_t i) {
    const uint8_t *eref = vec + 4u + i * 4u;
    if (eref + 4u > fb + fb_size)
        return NULL;
    uint32_t uoff;
    blyt32_native_memcpy(&uoff, eref, 4);
    const uint8_t *elem = eref + uoff;
    if (elem < fb || elem + 4u > fb + fb_size)
        return NULL;
    return elem;
}

/* A FlatBuffer string field (voff): returns the byte pointer + length; NULL if absent. */
static const char *fb_string(const uint8_t *fb, uint32_t fb_size, const uint8_t *table,
                             uint16_t voff, uint32_t *out_len) {
    const uint8_t *s = fb_uoffset_field(fb, fb_size, table, voff);
    if (!s)
        return NULL;
    uint32_t len;
    blyt32_native_memcpy(&len, s, 4);
    const char *str = (const char *)(s + 4);
    if (str + len > (const char *)(fb + fb_size))
        return NULL;
    *out_len = len;
    return str;
}

/* Inline u8 scalar field (voff); `dflt` if absent. */
static uint8_t fb_u8_field(const uint8_t *fb, uint32_t fb_size, const uint8_t *table, uint16_t voff,
                           uint8_t dflt) {
    uint16_t foff = fb_field_off(fb, fb_size, table, voff);
    if (foff == 0)
        return dflt;
    const uint8_t *p = table + foff;
    if (p + 1u > fb + fb_size)
        return dflt;
    return *p;
}

/* Inline u64 scalar field (voff); `dflt` if absent. */
static uint64_t fb_u64_field(const uint8_t *fb, uint32_t fb_size, const uint8_t *table,
                             uint16_t voff, uint64_t dflt) {
    uint16_t foff = fb_field_off(fb, fb_size, table, voff);
    if (foff == 0)
        return dflt;
    const uint8_t *p = table + foff;
    if (p + 8u > fb + fb_size)
        return dflt;
    uint64_t v;
    blyt32_native_memcpy(&v, p, 8);
    return v;
}

static int fb_str_eq(const char *a, uint32_t alen, const char *b, uint32_t blen) {
    if (alen != blen)
        return 0;
    for (uint32_t i = 0; i < alen; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

static void load_cart_buf_counts(void) {
    /* Safe defaults until/unless the ELF is parsed successfully.  Generations
     * start at 1 (unbiased; ADR-0096) — set explicitly since no .init_array
     * constructors run on this path (#43), matching the host (state_buffer.c). */
    for (uint32_t bi = 0; bi < NATIVE_MAX_BUF; bi++) {
        s_buf_count[bi] = NATIVE_MAX_SLOTS;
        for (uint32_t s = 0; s < NATIVE_MAX_SLOTS; s++)
            s_gen[bi][s] = 1;
    }

    const char *path = getenv("BLYT_CART_PATH");
    if (!path || !path[0])
        return;

    int fd = blyt_rs_openat(NATIVE_AT_FDCWD, path, NATIVE_O_RDONLY, 0);
    if (fd < 0)
        return;

    /* Get file size via statx (syscall 291; allowed by launcher RV32 filter).
     * struct statx is 256 bytes; stx_size (uint64_t) is at byte offset 40. */
    char stx[256];
    blyt32_native_memset(stx, 0, sizeof(stx));
    {
        register long a0 __asm__("a0") = NATIVE_AT_FDCWD;
        register const char *a1 __asm__("a1") = path;
        register long a2 __asm__("a2") = 0; /* AT_STATX_SYNC_AS_STAT */
        register long a3 __asm__("a3") = 0x200; /* STATX_SIZE */
        register char *a4 __asm__("a4") = stx;
        register long a7 __asm__("a7") = 291; /* SYS_statx */
        __asm__ volatile("ecall"
                         : "+r"(a0)
                         : "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a7)
                         : "memory");
        if (a0 != 0) {
            blyt_rs_close(fd);
            return;
        }
    }
    /* stx_size (uint64_t LE) at offset 40; low 32 bits suffice for any cart. */
    uint32_t file_size;
    blyt32_native_memcpy(&file_size, stx + 40, 4);
    if (file_size < 52u) { /* smaller than an ELF32 header */
        blyt_rs_close(fd);
        return;
    }

    /* Map the cart ELF read-only (syscall 222; allowed before restricted filter). */
    void *map;
    {
        register long a0 __asm__("a0") = 0; /* addr = NULL */
        register long a1 __asm__("a1") = file_size;
        register long a2 __asm__("a2") = 1; /* PROT_READ */
        register long a3 __asm__("a3") = 2; /* MAP_PRIVATE */
        register long a4 __asm__("a4") = fd;
        register long a5 __asm__("a5") = 0; /* offset = 0 */
        register long a7 __asm__("a7") = 222; /* SYS_mmap */
        __asm__ volatile("ecall"
                         : "+r"(a0)
                         : "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a7)
                         : "memory");
        map = (void *)a0;
    }
    blyt_rs_close(fd);
    /* mmap error: low 32 bits ≥ 0xFFFFF000 (last 4 KiB of address space). */
    if ((uint32_t)(unsigned long)map >= 0xFFFFF000u)
        return;

    const uint8_t *m = (const uint8_t *)map;

    /* ── Parse .cart.config save_version (ADR-0125) ──
     * Independent of .cart.layouts: every cart emits .cart.config, but a cart
     * with no state buffers has no .cart.layouts.  Stamped into the save header
     * on write, reported as saved_cart_version on read. */
    {
        uint32_t config_off = 0, config_size = 0;
        if (blyt_elf32_find_section(m, file_size, ".cart.config", &config_off, &config_size) &&
            config_size > 8u) {
            /* Skip 8-byte CCFG preamble ("CCFG" + u16 major + u16 minor). */
            const uint8_t *cfb = m + config_off + 8;
            uint32_t cfb_size = config_size - 8u;
            /* Inline FlatBuffer scalar read: CartConfig.save_version is field
             * id 1 (vtable byte offset 6), a uint32; default 0 if absent. */
            if (cfb_size >= 8u) {
                uint32_t croot;
                blyt32_native_memcpy(&croot, cfb, 4);
                if (croot + 4u <= cfb_size) {
                    const uint8_t *ctable = cfb + croot;
                    int32_t cvsoff;
                    blyt32_native_memcpy(&cvsoff, ctable, 4);
                    uint32_t ctable_off = (uint32_t)(ctable - cfb);
                    if (cvsoff > 0 && (uint32_t)cvsoff <= ctable_off) {
                        const uint8_t *cvtable = ctable - (uint32_t)cvsoff;
                        uint16_t cvtsize;
                        blyt32_native_memcpy(&cvtsize, cvtable, 2);
                        if (cvtsize >= 8u && (uint32_t)(cvtable - cfb) + cvtsize <= cfb_size) {
                            uint16_t sv_foff;
                            blyt32_native_memcpy(&sv_foff, cvtable + 6, 2);
                            if (sv_foff != 0) {
                                const uint8_t *sv_ptr = ctable + sv_foff;
                                if (sv_ptr + 4u <= cfb + cfb_size)
                                    blyt32_native_memcpy(&s_save_version, sv_ptr, 4);
                            }
                        }
                    }
                }
            }
        }
    }

    /* ── Parse .cart.layouts buffer counts ── */
    uint32_t layouts_off = 0, layouts_size = 0;
    if (!blyt_elf32_find_section(m, file_size, ".cart.layouts", &layouts_off, &layouts_size) ||
        layouts_size <= 8u)
        goto done;

    /* Skip 8-byte CLAY preamble ("CLAY" + u16 major + u16 minor). */
    const uint8_t *fb = m + layouts_off + 8;
    uint32_t fb_size = layouts_size - 8u;
    if (fb_size < 8u)
        goto done;

    /* ── Inline FlatBuffer parse (CartLayouts root) ── */
    /* Root uoffset: *(u32*)fb → CartLayouts table address. */
    uint32_t root_off;
    blyt32_native_memcpy(&root_off, fb, 4);
    if (root_off + 4u > fb_size)
        goto done;
    const uint8_t *table = fb + root_off;

    /* Vtable: soffset at table[0..3]; vtable = table - soffset.
     * soffset = table_addr - vtable_addr (positive ⇒ vtable is before table). */
    int32_t vtab_soff;
    blyt32_native_memcpy(&vtab_soff, table, 4);
    uint32_t table_fb_off = (uint32_t)(table - fb);
    if (vtab_soff <= 0 || (uint32_t)vtab_soff > table_fb_off)
        goto done;
    const uint8_t *vtable = table - (uint32_t)vtab_soff;
    /* Vtable: vtsize(u16), objsize(u16), then field offsets (u16 each).
     * CartLayouts field 1 (buffers vector) is at vtable byte offset 6. */
    uint16_t vtsize;
    blyt32_native_memcpy(&vtsize, vtable, 2);
    if (vtsize < 8u || (uint32_t)(vtable - fb) + vtsize > fb_size)
        goto done;
    uint16_t buffers_foff; /* field offset within the table object */
    blyt32_native_memcpy(&buffers_foff, vtable + 6, 2);
    if (buffers_foff == 0)
        goto done; /* buffers field not present */

    /* Field value: uoffset at table + buffers_foff, relative to itself. */
    const uint8_t *fptr = table + buffers_foff;
    if (fptr + 4u > fb + fb_size)
        goto done;
    uint32_t vec_uoff;
    blyt32_native_memcpy(&vec_uoff, fptr, 4);
    const uint8_t *vec = fptr + vec_uoff;
    if (vec + 4u > fb + fb_size)
        goto done;

    /* Vector: u32 length, then u32 element uoffsets (each relative to itself). */
    uint32_t vec_len;
    blyt32_native_memcpy(&vec_len, vec, 4);
    uint32_t n = vec_len < NATIVE_MAX_BUF ? vec_len : NATIVE_MAX_BUF;
    s_n_buffers = n; /* CART sections emitted, in declaration order (#129) */

    /* CartLayouts.schema_hash (field 2, vtable offset 8, u64) — stamped into the
     * BLYS header and checked on read (#129). */
    s_schema_hash = fb_u64_field(fb, fb_size, table, 8, 0);

    /* records[] vector (CartLayouts field 0, vtable offset 4) — resolved once,
     * then matched per buffer by record_name to flatten field type tags (#134). */
    const uint8_t *rec_vec = fb_uoffset_field(fb, fb_size, table, 4);
    uint32_t n_recs = 0;
    if (rec_vec)
        blyt32_native_memcpy(&n_recs, rec_vec, 4);

    for (uint32_t bi = 0; bi < n; bi++) {
        const uint8_t *eref = vec + 4u + bi * 4u;
        if (eref + 4u > fb + fb_size)
            break;
        uint32_t elem_uoff;
        blyt32_native_memcpy(&elem_uoff, eref, 4);
        const uint8_t *elem = eref + elem_uoff;
        if (elem + 4u > fb + fb_size)
            continue;

        /* BufferDecl vtable: field 2 (count) at vtable byte offset 8. */
        int32_t e_soff;
        blyt32_native_memcpy(&e_soff, elem, 4);
        uint32_t elem_fb_off = (uint32_t)(elem - fb);
        if (e_soff <= 0 || (uint32_t)e_soff > elem_fb_off)
            continue;
        const uint8_t *e_vtable = elem - (uint32_t)e_soff;
        uint16_t e_vtsize;
        blyt32_native_memcpy(&e_vtsize, e_vtable, 2);
        if (e_vtsize < 10u || (uint32_t)(e_vtable - fb) + e_vtsize > fb_size)
            continue;
        uint16_t count_foff;
        blyt32_native_memcpy(&count_foff, e_vtable + 8, 2);
        if (count_foff == 0)
            continue; /* count field absent; leave default */
        const uint8_t *count_ptr = elem + count_foff;
        if (count_ptr + 4u > fb + fb_size)
            continue;
        uint32_t count;
        blyt32_native_memcpy(&count, count_ptr, 4);
        s_buf_count[bi] = count < NATIVE_MAX_SLOTS ? count : NATIVE_MAX_SLOTS;

        /* Buffer name (BufferDecl.name, field 0, vtable offset 4) — copied out
         * (the mmap is released before save time) for the CART section (#129). */
        uint32_t bn_len = 0;
        const char *bn = fb_string(fb, fb_size, elem, 4, &bn_len);
        if (bn) {
            if (bn_len > NATIVE_BUF_NAME_CAP - 1u)
                bn_len = NATIVE_BUF_NAME_CAP - 1u;
            blyt32_native_memcpy(s_buf_name[bi], bn, bn_len);
            s_buf_name[bi][bn_len] = '\0';
            s_buf_name_len[bi] = bn_len;
        }

        /* Flatten this buffer's field type tags (#134): follow BufferDecl
         * .record_name (field 1, vtable offset 6) into records[], match by
         * name, then read each FieldDecl.type_tag (field 1, vtable offset 6).
         * On any miss s_field_count[bi] stays 0 (no save bytes / no zeroing). */
        uint32_t rn_len = 0;
        const char *rn = fb_string(fb, fb_size, elem, 6, &rn_len);
        if (!rn)
            continue;
        for (uint32_t ri = 0; ri < n_recs; ri++) {
            const uint8_t *rd = fb_vec_at(fb, fb_size, rec_vec, ri);
            if (!rd)
                continue;
            uint32_t rdn_len = 0;
            const char *rdn = fb_string(fb, fb_size, rd, 4, &rdn_len); /* RecordDecl.name */
            if (!rdn || !fb_str_eq(rn, rn_len, rdn, rdn_len))
                continue;
            const uint8_t *fld_vec = fb_uoffset_field(fb, fb_size, rd, 6); /* RecordDecl.fields */
            if (!fld_vec)
                break;
            uint32_t n_fields;
            blyt32_native_memcpy(&n_fields, fld_vec, 4);
            if (n_fields > NATIVE_MAX_FIELDS)
                n_fields = NATIVE_MAX_FIELDS;
            for (uint32_t fi = 0; fi < n_fields; fi++) {
                const uint8_t *fd = fb_vec_at(fb, fb_size, fld_vec, fi);
                if (!fd)
                    break;
                /* type_tag default is 0 (i8): FlatBuffers omits a field equal
                 * to its schema default, so an absent tag means i8, not i32. */
                s_field_type[bi][fi] = fb_u8_field(fb, fb_size, fd, 6, NATIVE_TYPE_I8);
                s_field_count[bi] = fi + 1u;
            }
            break;
        }
    }

done:;
    register long a0 __asm__("a0") = (long)map;
    register long a1 __asm__("a1") = file_size;
    register long a7 __asm__("a7") = 215; /* SYS_munmap */
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a7) : "memory");
}

/* ── Resource lifecycle (ADR-0027, #123) ─────────────────────────────────────
 *
 * Native bare-metal serves resources from the cart's own packed ELF: the cart
 * file is mmap'd read-only at startup (before the seccomp filter, like
 * load_cart_buf_counts) and retained, so pin can hand back a direct pointer
 * into that mapping — no per-frame copy, no syscall.  Per-id refcounts use the
 * shared state machine (runtime/shared) so native and the host cannot drift.
 * Only embedded `.cart.resource.<id>` sections are served; the dev-mode staging
 * directory is an emulated-path-only concern. */

/* The resource table is sized from the cart's actual `.cart.resource.<id>`
 * section count at startup — no fixed compile-time cap; the bound is memory
 * (#141).  It is malloc'd once from the pre-filter window (mmap, syscall 222, is
 * on the restricted allowlist, so malloc works on this path too) and fully
 * populated, then never grown — deterministic and matching the host's eager
 * load model (resource.c blyt_resource_table_load_from_cart). */
typedef struct {
    uint32_t id;
    const uint8_t *data; /* points into s_cart_map */
    uint32_t len;
    blyt_rl_state_t rl;
} native_res_t;

static const uint8_t *s_cart_map; /* retained read-only cart mapping, or NULL */
static uint32_t s_cart_size;
static native_res_t *s_res; /* malloc'd array of s_res_count entries, or NULL */
static uint32_t s_res_count;

/* malloc/free come from ld-blyt.so.1 (system musl) via the DT_NEEDED chain; the
 * native libblytcommon has no other libc dependency declared, so declare them
 * here rather than pulling in <stdlib.h>. */
extern void *malloc(size_t);

/* ── Shared `.cart.resource.<id>` enumeration (runtime/shared, #141) ──
 * Two-pass eager load: count the numeric resource sections, allocate exactly
 * that many entries, then fill them.  The id parse and section walk are shared
 * with the host (blyt_elf32_for_each_section_prefix / blyt_elf32_parse_u32) so
 * resource discovery cannot drift between the two paths. */
#define NATIVE_RES_PREFIX ".cart.resource."

static void res_count_cb(const char *suffix, uint32_t suffix_len, uint32_t off, uint32_t size,
                         void *vctx) {
    (void)off;
    (void)size;
    uint32_t id;
    if (blyt_elf32_parse_u32(suffix, suffix_len, &id))
        (*(uint32_t *)vctx)++;
}

struct res_fill_ctx {
    native_res_t *arr;
    uint32_t cap;
    uint32_t n;
    const uint8_t *map;
};

static void res_fill_cb(const char *suffix, uint32_t suffix_len, uint32_t off, uint32_t size,
                        void *vctx) {
    struct res_fill_ctx *c = (struct res_fill_ctx *)vctx;
    uint32_t id;
    if (!blyt_elf32_parse_u32(suffix, suffix_len, &id))
        return;
    if (c->n >= c->cap)
        return; /* defensive: the count and fill passes see the same sections */
    c->arr[c->n].id = id;
    c->arr[c->n].data = c->map + off;
    c->arr[c->n].len = size;
    c->arr[c->n].rl = (blyt_rl_state_t){0};
    c->n++;
}

/* mmap the cart ELF read-only and RETAIN it (no munmap): pin returns pointers
 * into this mapping for the cart's lifetime.  Must run before the restricted
 * seccomp filter (mmap/statx are blocked afterwards). */
static void resources_init(void) {
    const char *path = getenv("BLYT_CART_PATH");
    if (!path || !path[0])
        return;
    int fd = blyt_rs_openat(NATIVE_AT_FDCWD, path, NATIVE_O_RDONLY, 0);
    if (fd < 0)
        return;

    char stx[256];
    blyt32_native_memset(stx, 0, sizeof(stx));
    {
        register long a0 __asm__("a0") = NATIVE_AT_FDCWD;
        register const char *a1 __asm__("a1") = path;
        register long a2 __asm__("a2") = 0; /* AT_STATX_SYNC_AS_STAT */
        register long a3 __asm__("a3") = 0x200; /* STATX_SIZE */
        register char *a4 __asm__("a4") = stx;
        register long a7 __asm__("a7") = 291; /* SYS_statx */
        __asm__ volatile("ecall"
                         : "+r"(a0)
                         : "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a7)
                         : "memory");
        if (a0 != 0) {
            blyt_rs_close(fd);
            return;
        }
    }
    uint32_t file_size;
    blyt32_native_memcpy(&file_size, stx + 40, 4);
    if (file_size < 52u) {
        blyt_rs_close(fd);
        return;
    }

    void *map;
    {
        register long a0 __asm__("a0") = 0; /* addr = NULL */
        register long a1 __asm__("a1") = file_size;
        register long a2 __asm__("a2") = 1; /* PROT_READ */
        register long a3 __asm__("a3") = 2; /* MAP_PRIVATE */
        register long a4 __asm__("a4") = fd;
        register long a5 __asm__("a5") = 0; /* offset = 0 */
        register long a7 __asm__("a7") = 222; /* SYS_mmap */
        __asm__ volatile("ecall"
                         : "+r"(a0)
                         : "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a7)
                         : "memory");
        map = (void *)a0;
    }
    blyt_rs_close(fd);
    if ((uint32_t)(unsigned long)map >= 0xFFFFF000u)
        return; /* mmap error */
    s_cart_map = (const uint8_t *)map;
    s_cart_size = file_size;

    /* Size the resource table from the cart's actual `.cart.resource.<id>`
     * section count and fully populate it (#141).  Two passes over the shared
     * ELF walk: count, then fill.  On OOM (or zero resources) the table stays
     * empty and every lookup returns NOT_FOUND — never a stale pointer. */
    uint32_t n = 0;
    blyt_elf32_for_each_section_prefix(s_cart_map, s_cart_size, NATIVE_RES_PREFIX, res_count_cb,
                                       &n);
    if (n == 0)
        return;
    native_res_t *arr = (native_res_t *)malloc((size_t)n * sizeof(native_res_t));
    if (!arr)
        return;
    struct res_fill_ctx fc = {arr, n, 0, s_cart_map};
    blyt_elf32_for_each_section_prefix(s_cart_map, s_cart_size, NATIVE_RES_PREFIX, res_fill_cb,
                                       &fc);
    s_res = arr;
    s_res_count = fc.n;
}

/* Look up a resource entry by id in the eagerly-populated table; -1 if absent. */
static int res_find(uint32_t id) {
    for (uint32_t i = 0; i < s_res_count; i++)
        if (s_res[i].id == id)
            return (int)i;
    return -1;
}

blyt_result_t blyt_resource_pin(blyt_resource_id_t id, const void **out_ptr, size_t *out_size) {
    int i = res_find(id);
    if (i < 0) {
        if (out_ptr)
            *out_ptr = 0;
        return BLYT_ERR_NOT_FOUND;
    }
    blyt_rl_pin(&s_res[i].rl);
    if (out_ptr)
        *out_ptr = s_res[i].data; /* direct pointer into the retained mmap */
    if (out_size)
        *out_size = s_res[i].len;
    return BLYT_OK;
}

blyt_result_t blyt_resource_unpin(blyt_resource_id_t id) {
    int i = res_find(id);
    if (i < 0)
        return BLYT_ERR_NOT_FOUND;
    return blyt_rl_unpin(&s_res[i].rl) ? BLYT_OK : BLYT_ERR_INVALID_ARG;
}

blyt_result_t blyt_resource_load(blyt_resource_id_t id, blyt_resource_h *out_handle) {
    int i = res_find(id);
    if (i < 0) {
        if (out_handle)
            *out_handle = BLYT_RESOURCE_INVALID;
        return BLYT_ERR_NOT_FOUND;
    }
    blyt_resource_h h = blyt_rl_load(&s_res[i].rl, id);
    if (out_handle)
        *out_handle = h;
    return BLYT_OK;
}

blyt_result_t blyt_resource_release(blyt_resource_h handle) {
    if (!handle)
        return BLYT_ERR_INVALID_ARG;
    int i = res_find(handle & 0xFFFFu);
    if (i < 0)
        return BLYT_ERR_INVALID_ARG;
    return blyt_rl_release(&s_res[i].rl, handle) ? BLYT_OK : BLYT_ERR_INVALID_ARG;
}

/* Frame-boundary force-release: drop every pin (ADR-0027 frame-scope). */
static void resources_force_release_pins(void) {
    for (uint32_t i = 0; i < s_res_count; i++)
        blyt_rl_force_release_pins(&s_res[i].rl);
}

void blyt_runtime_startup(void) {
    load_cart_buf_counts(); /* must precede restricted filter (uses statx/mmap) */
    resources_init(); /* same: retains a cart mmap before the filter (#123) */
    if (blyt_install_restricted_filter() != 0) {
        static const char msg[] = "libblytcommon: FATAL: seccomp install failed\n";
        blyt_rs_write(2, msg, sizeof(msg) - 1);
        blyt_rs_exit_group(127);
    }
    /* Explicitly reset FCSR to RNE+no-flags regardless of OS/ld.so state. */
    __asm__ volatile("csrw fcsr, zero" ::: "memory");
}

/* ── Native API implementations ──────────────────────────────────────────
 *
 * Strong definitions in the native libblytcommon.so variant.  On the native
 * path this variant is loaded in place of the emulated one (recompiled into
 * sdk/lib/native/, not copied), so these definitions are the ones the cart
 * resolves over the DT_NEEDED chain.
 */

/* blyt_frame_done — frame-boundary housekeeping on the native path.
 *
 * Called by blyt_main at the end of each logical frame.  Enforces FP
 * determinism (ADR-0007) by checking and resetting the RISC-V FCSR:
 *
 *   frm (bits 7:5) — rounding mode.  Must be 0 (round-to-nearest-even,
 *   the IEEE 754 default) at every frame boundary.  A non-zero frm means
 *   the cart or one of its libraries called fesetround() or modified frm
 *   directly, which would cause FP results to diverge across runs.
 *
 *   fflags (bits 4:0) — accumulated FP exception flags (NX/UF/OF/DZ/NV).
 *   These are set by normal FP arithmetic and do not affect determinism;
 *   they are cleared here to give each frame a clean starting state.
 *
 * Debug builds emit a warning and continue; release builds abort because
 * a dirty frm means results from this frame are already non-deterministic
 * and allowing the cart to continue would compound the divergence. */
void blyt_frame_done(void) {
    unsigned int fcsr;
    __asm__ volatile("csrr %0, fcsr" : "=r"(fcsr));
    unsigned int frm = (fcsr >> 5) & 0x7u;
    if (frm != 0u) {
#ifndef NDEBUG
        static const char pfx[] = "blyt: WARNING: cart set non-default FP rounding mode (frm=";
        static const char sfx[] = "); results may be non-deterministic\n";
        char digit = (char)('0' + frm);
        blyt_rs_write(2, pfx, sizeof(pfx) - 1);
        blyt_rs_write(2, &digit, 1);
        blyt_rs_write(2, sfx, sizeof(sfx) - 1);
#else
        static const char msg[] = "blyt: cart set non-default FP rounding mode; "
                                  "aborting for determinism\n";
        blyt_rs_write(2, msg, sizeof(msg) - 1);
        blyt_rs_exit_group(1);
#endif
    }
    /* Reset frm to RNE (0) and clear accumulated fflags for the next frame.
     * The memory clobber prevents the compiler from reordering FP operations
     * across this boundary. */
    __asm__ volatile("csrw fcsr, zero" ::: "memory");
    /* Force-release any resource pins still held: a pin is valid only within the
     * frame it was taken (ADR-0027 frame-scope amendment, #123). */
    resources_force_release_pins();
    if (blyt32_trace_api_enabled()) {
        static const char msg[] = "[blyt:api] frame_done()\n";
        blyt32_trace_write(msg, sizeof(msg) - 1);
    }
}

/* blyt_exit — clean process exit after the cart main loop.
 *
 * Called by _blyt_entry (the ELF entry-point stub) after blyt_main() returns.
 * exit_group(2) (NR 94) is the same mechanism on both paths — the host
 * emulator intercepts the ecall on the emulated path, the kernel handles it on
 * the native path — so this is variant-agnostic.  It bypasses musl's exit()
 * cleanup, which would call syscalls the restricted seccomp filter blocks.
 *
 * __attribute__((noreturn)) lets the compiler omit the return path in
 * _blyt_entry and avoid a dead-code epilogue. */
__attribute__((noreturn)) void blyt_exit(int code) {
    blyt32_trace_call("exit", (long)code, 0, 0);
    blyt_rs_exit_group(code);
}

/* blyt_console_debug — SYS_write(fd=2, s, len).
 * write(2) is NR 64, in the restricted allowlist. */
void blyt_console_debug(const char *s) {
    unsigned int len = blyt32_native_strlen(s);
    if (blyt32_trace_api_enabled()) {
        char tbuf[300];
        char *tend = tbuf + sizeof(tbuf);
        char *tp = blyt32_trace_app_str(tbuf, tend, "[blyt:api] console_debug(\"");
        unsigned int tl = len;
        while (tl > 0 && (s[tl - 1] == '\n' || s[tl - 1] == '\r'))
            tl--;
        for (unsigned int ti = 0; ti < tl && tp + 1 < tend; ti++)
            *tp++ = s[ti];
        tp = blyt32_trace_app_str(tp, tend, "\")");
        blyt32_trace_emit(tbuf, tp, tend);
    }
    register long a0 __asm__("a0") = 2; /* STDERR_FILENO */
    register const char *a1 __asm__("a1") = s;
    register long a2 __asm__("a2") = len;
    register long a7 __asm__("a7") = 64; /* SYS_write */
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
}

/* ── State buffer typed get/set (Phase 9; field-major storage #134) ───────
 *
 * Buffer handle (buf_h): 1-based buffer index.
 * Field handle (field_h): upper 16 bits = buf_id (must match buf_h),
 *                          lower 16 bits = 1-based field index.
 * Each value is stored at its true width in the field-major arena: element s
 * of buffer bi field fi lives at field_base(bi, fi) + s*width.  Each accessor
 * reads/writes its own intrinsic width (get_i32 → 4 bytes, get_i8 → 1, etc.). */

float blyt_buffer_get_f32(blyt_buffer_h b, int32_t s, blyt_field_h f) {
    uint32_t bi = b - 1u, fi = (f & 0xFFFFu) - 1u;
    if (!ref_ok(bi, s, fi))
        return 0.0f;
    uint32_t bits;
    blyt32_native_memcpy(&bits, field_base(bi, fi) + (uint32_t)s * 4u, 4);
    blyt32_trace_buf_op("buf_get_f32", b, s, f, bits, 0, 1);
    float v;
    blyt32_native_memcpy(&v, &bits, 4);
    return v;
}
void blyt_buffer_set_f32(blyt_buffer_h b, int32_t s, blyt_field_h f, float v) {
    uint32_t bi = b - 1u, fi = (f & 0xFFFFu) - 1u;
    if (!ref_ok(bi, s, fi))
        return;
    uint32_t bits;
    blyt32_native_memcpy(&bits, &v, 4);
    blyt32_trace_buf_op("buf_set_f32", b, s, f, bits, 1, 1);
    bits = blyt_canon_f32(bits);
    blyt32_native_memcpy(field_base(bi, fi) + (uint32_t)s * 4u, &bits, 4);
}

/* f64 (Spike U): full 64-bit field; 8-byte-aligned sub-array in the arena. */
double blyt_buffer_get_f64(blyt_buffer_h b, int32_t s, blyt_field_h f) {
    uint32_t bi = b - 1u, fi = (f & 0xFFFFu) - 1u;
    if (!ref_ok(bi, s, fi))
        return 0.0;
    uint64_t bits;
    blyt32_native_memcpy(&bits, field_base(bi, fi) + (uint32_t)s * 8u, 8);
    double v;
    blyt32_native_memcpy(&v, &bits, 8);
    return v;
}
void blyt_buffer_set_f64(blyt_buffer_h b, int32_t s, blyt_field_h f, double v) {
    uint32_t bi = b - 1u, fi = (f & 0xFFFFu) - 1u;
    if (!ref_ok(bi, s, fi))
        return;
    uint64_t bits;
    blyt32_native_memcpy(&bits, &v, 8);
    bits = blyt_canon_f64(bits);
    blyt32_native_memcpy(field_base(bi, fi) + (uint32_t)s * 8u, &bits, 8);
}

int32_t blyt_buffer_get_i32(blyt_buffer_h b, int32_t s, blyt_field_h f) {
    uint32_t bi = b - 1u, fi = (f & 0xFFFFu) - 1u;
    if (!ref_ok(bi, s, fi))
        return 0;
    uint32_t bits;
    blyt32_native_memcpy(&bits, field_base(bi, fi) + (uint32_t)s * 4u, 4);
    blyt32_trace_buf_op("buf_get_i32", b, s, f, bits, 0, 0);
    return (int32_t)bits;
}
void blyt_buffer_set_i32(blyt_buffer_h b, int32_t s, blyt_field_h f, int32_t v) {
    uint32_t bi = b - 1u, fi = (f & 0xFFFFu) - 1u;
    if (!ref_ok(bi, s, fi))
        return;
    blyt32_trace_buf_op("buf_set_i32", b, s, f, (uint32_t)v, 1, 0);
    uint32_t bits = (uint32_t)v;
    blyt32_native_memcpy(field_base(bi, fi) + (uint32_t)s * 4u, &bits, 4);
}

uint32_t blyt_buffer_get_u32(blyt_buffer_h b, int32_t s, blyt_field_h f) {
    uint32_t bi = b - 1u, fi = (f & 0xFFFFu) - 1u;
    if (!ref_ok(bi, s, fi))
        return 0u;
    uint32_t bits;
    blyt32_native_memcpy(&bits, field_base(bi, fi) + (uint32_t)s * 4u, 4);
    blyt32_trace_buf_op("buf_get_u32", b, s, f, bits, 0, 0);
    return bits;
}
void blyt_buffer_set_u32(blyt_buffer_h b, int32_t s, blyt_field_h f, uint32_t v) {
    uint32_t bi = b - 1u, fi = (f & 0xFFFFu) - 1u;
    if (!ref_ok(bi, s, fi))
        return;
    blyt32_trace_buf_op("buf_set_u32", b, s, f, v, 1, 0);
    blyt32_native_memcpy(field_base(bi, fi) + (uint32_t)s * 4u, &v, 4);
}

int16_t blyt_buffer_get_i16(blyt_buffer_h b, int32_t s, blyt_field_h f) {
    uint32_t bi = b - 1u, fi = (f & 0xFFFFu) - 1u;
    if (!ref_ok(bi, s, fi))
        return 0;
    int16_t v16;
    blyt32_native_memcpy(&v16, field_base(bi, fi) + (uint32_t)s * 2u, 2);
    blyt32_trace_buf_op("buf_get_i16", b, s, f, (uint32_t)(int32_t)v16, 0, 0);
    return v16;
}
void blyt_buffer_set_i16(blyt_buffer_h b, int32_t s, blyt_field_h f, int16_t v) {
    uint32_t bi = b - 1u, fi = (f & 0xFFFFu) - 1u;
    if (!ref_ok(bi, s, fi))
        return;
    blyt32_trace_buf_op("buf_set_i16", b, s, f, (uint32_t)(int32_t)v, 1, 0);
    uint16_t raw = (uint16_t)v;
    blyt32_native_memcpy(field_base(bi, fi) + (uint32_t)s * 2u, &raw, 2);
}

uint16_t blyt_buffer_get_u16(blyt_buffer_h b, int32_t s, blyt_field_h f) {
    uint32_t bi = b - 1u, fi = (f & 0xFFFFu) - 1u;
    if (!ref_ok(bi, s, fi))
        return 0u;
    uint16_t v16;
    blyt32_native_memcpy(&v16, field_base(bi, fi) + (uint32_t)s * 2u, 2);
    blyt32_trace_buf_op("buf_get_u16", b, s, f, (uint32_t)v16, 0, 0);
    return v16;
}
void blyt_buffer_set_u16(blyt_buffer_h b, int32_t s, blyt_field_h f, uint16_t v) {
    uint32_t bi = b - 1u, fi = (f & 0xFFFFu) - 1u;
    if (!ref_ok(bi, s, fi))
        return;
    blyt32_trace_buf_op("buf_set_u16", b, s, f, (uint32_t)v, 1, 0);
    blyt32_native_memcpy(field_base(bi, fi) + (uint32_t)s * 2u, &v, 2);
}

int8_t blyt_buffer_get_i8(blyt_buffer_h b, int32_t s, blyt_field_h f) {
    uint32_t bi = b - 1u, fi = (f & 0xFFFFu) - 1u;
    if (!ref_ok(bi, s, fi))
        return 0;
    int8_t v8;
    blyt32_native_memcpy(&v8, field_base(bi, fi) + (uint32_t)s, 1);
    blyt32_trace_buf_op("buf_get_i8", b, s, f, (uint32_t)(int32_t)v8, 0, 0);
    return v8;
}
void blyt_buffer_set_i8(blyt_buffer_h b, int32_t s, blyt_field_h f, int8_t v) {
    uint32_t bi = b - 1u, fi = (f & 0xFFFFu) - 1u;
    if (!ref_ok(bi, s, fi))
        return;
    blyt32_trace_buf_op("buf_set_i8", b, s, f, (uint32_t)(int32_t)v, 1, 0);
    uint8_t raw = (uint8_t)v;
    blyt32_native_memcpy(field_base(bi, fi) + (uint32_t)s, &raw, 1);
}

uint8_t blyt_buffer_get_u8(blyt_buffer_h b, int32_t s, blyt_field_h f) {
    uint32_t bi = b - 1u, fi = (f & 0xFFFFu) - 1u;
    if (!ref_ok(bi, s, fi))
        return 0u;
    uint8_t v8;
    blyt32_native_memcpy(&v8, field_base(bi, fi) + (uint32_t)s, 1);
    blyt32_trace_buf_op("buf_get_u8", b, s, f, (uint32_t)v8, 0, 0);
    return v8;
}
void blyt_buffer_set_u8(blyt_buffer_h b, int32_t s, blyt_field_h f, uint8_t v) {
    uint32_t bi = b - 1u, fi = (f & 0xFFFFu) - 1u;
    if (!ref_ok(bi, s, fi))
        return;
    blyt32_trace_buf_op("buf_set_u8", b, s, f, (uint32_t)v, 1, 0);
    blyt32_native_memcpy(field_base(bi, fi) + (uint32_t)s, &v, 1);
}

bool blyt_buffer_get_bool(blyt_buffer_h b, int32_t s, blyt_field_h f) {
    uint32_t bi = b - 1u, fi = (f & 0xFFFFu) - 1u;
    if (!ref_ok(bi, s, fi))
        return false;
    uint8_t raw;
    blyt32_native_memcpy(&raw, field_base(bi, fi) + (uint32_t)s, 1);
    bool vb = raw != 0u;
    blyt32_trace_buf_op("buf_get_bool", b, s, f, vb ? 1u : 0u, 0, 0);
    return vb;
}
void blyt_buffer_set_bool(blyt_buffer_h b, int32_t s, blyt_field_h f, bool v) {
    uint32_t bi = b - 1u, fi = (f & 0xFFFFu) - 1u;
    if (!ref_ok(bi, s, fi))
        return;
    blyt32_trace_buf_op("buf_set_bool", b, s, f, v ? 1u : 0u, 1, 0);
    uint8_t raw = v ? 1u : 0u;
    blyt32_native_memcpy(field_base(bi, fi) + (uint32_t)s, &raw, 1);
}

/* ── Slot management ────────────────────────────────────────────────────── */

blyt_result_t blyt_buffer_alloc_slot(blyt_buffer_h b, int32_t *out_slot) {
    uint32_t bi = b - 1u;
    if (bi >= NATIVE_MAX_BUF || !out_slot)
        return BLYT_ERR_INVALID_ARG;
    for (int32_t i = 0; i < (int32_t)s_buf_count[bi]; i++) {
        uint32_t byte = (uint32_t)i / 8u, bit = (uint32_t)i % 8u;
        if (!(s_slot_bits[bi][byte] & (uint8_t)(1u << bit))) {
            s_slot_bits[bi][byte] |= (uint8_t)(1u << bit);
            *out_slot = i;
            blyt32_trace_call("buf_alloc_slot", (long)b, 1, (long)i);
            return BLYT_OK;
        }
    }
    blyt32_trace_call("buf_alloc_slot", (long)b, 1, -1);
    return BLYT_ERR_BUFFER_FULL;
}

blyt_result_t blyt_buffer_free_slot(blyt_buffer_h b, int32_t s) {
    uint32_t bi = b - 1u;
    /* Match the emulated path (state_buffer.c): freeing an unallocated slot
     * is an error and must not bump the generation counter. */
    if (bi >= NATIVE_MAX_BUF || s < 0 || (uint32_t)s >= s_buf_count[bi] || !slot_allocated(bi, s))
        return BLYT_ERR_INVALID_ARG;
    blyt32_trace_call("buf_free_slot", (long)s, 0, 0);
    s_slot_bits[bi][(uint32_t)s / 8u] &= (uint8_t)~(1u << ((uint32_t)s % 8u));
    /* Zero the freed slot's field data per field at its true width (the
     * emulated path does; host parity). */
    for (uint32_t fi = 0; fi < s_field_count[bi]; fi++) {
        uint32_t w = field_width(s_field_type[bi][fi]);
        blyt32_native_memset(field_base(bi, fi) + (uint32_t)s * w, 0, w);
    }
    /* Bump the generation (ADR-0096) via the shared primitive (unbiased storage,
     * wrapping 65535 -> 1), matching the host (state_buffer.c). */
    s_gen[bi][s] = blyt_gen_next(s_gen[bi][s]);
    return BLYT_OK;
}

/* ── Packed entity refs (ADR-0096) ──────────────────────────────────────────
 * blyt_buffer_ref_slot is a static inline in blyt.h (pure bit math). */

blyt_entity_ref_t blyt_buffer_ref(blyt_buffer_h b, int32_t s) {
    uint32_t bi = b - 1u;
    blyt_entity_ref_t ref = 0;
    if (bi < NATIVE_MAX_BUF && s >= 0 && (uint32_t)s < s_buf_count[bi] && slot_allocated(bi, s))
        ref = ((uint32_t)native_gen(bi, s) << 16) | (uint32_t)s;
    blyt32_trace_call("buf_ref", (long)s, 1, (long)ref);
    return ref;
}

bool blyt_buffer_ref_valid(blyt_buffer_h b, blyt_entity_ref_t ref) {
    uint32_t bi = b - 1u;
    int32_t s = (int32_t)(ref & 0xFFFFu);
    int v = ref != 0 && bi < NATIVE_MAX_BUF && (uint32_t)s < s_buf_count[bi] &&
            slot_allocated(bi, s) && native_gen(bi, s) == (uint16_t)(ref >> 16);
    blyt32_trace_call("buf_ref_valid", (long)ref, 1, (long)v);
    return v != 0;
}

/* ── Save / load (ADR-0125, #129) ──────────────────────────────────────────
 *
 * The on-disk byte layout (BLYS header + one CART section per buffer) is defined
 * once in runtime/shared/blyt_blys.{c,h} and shared with the host runtime, so
 * native and host saves are byte-identical and interchangeable.  Native only
 * adapts its storage (field-major arena, slot bitset, unbiased generations) into
 * the shared descriptor and provides raw-syscall I/O callbacks.
 *
 * BLYT_SAVE_DIR (set by the test runner) determines the directory; the file is
 * slot_<N>.blys within it.
 */

/* I/O callbacks for the shared serializer; ctx is &fd. */
static int native_sink(void *ctx, const void *buf, uint32_t len) {
    return write_all(*(const int *)ctx, buf, len);
}
static int native_source(void *ctx, void *buf, uint32_t len) {
    return read_all(*(const int *)ctx, buf, len);
}

/* Build the shared per-buffer views from native storage.  fd holds each
 * buffer's field-array base pointers (referenced by views[bi].field_data). */
static void fill_native_views(blys_buffer_t *views, void *fd[][NATIVE_MAX_FIELDS]) {
    for (uint32_t bi = 0; bi < s_n_buffers; bi++) {
        for (uint32_t fi = 0; fi < s_field_count[bi]; fi++)
            fd[bi][fi] = field_base(bi, fi);
        views[bi].name = s_buf_name[bi];
        views[bi].name_len = s_buf_name_len[bi];
        views[bi].n_fields = s_field_count[bi];
        views[bi].field_types = s_field_type[bi];
        views[bi].count = s_buf_count[bi];
        views[bi].field_data = fd[bi];
        views[bi].slot_bitset = s_slot_bits[bi];
        views[bi].slot_gens = s_gen[bi];
    }
}

blyt_result_t blyt_save_write(uint32_t slot) {
    blyt_cart_on_save_state();

    const char *save_dir = getenv("BLYT_SAVE_DIR");
    if (!save_dir || save_dir[0] == '\0') {
        static const char warn[] = "blyt: blyt_save_write: BLYT_SAVE_DIR not set\n";
        blyt_rs_write(2, warn, sizeof(warn) - 1);
        return BLYT_ERR_IO;
    }

    char path[512];
    build_save_path(path, sizeof(path), save_dir, slot);

    int fd = blyt_rs_openat(NATIVE_AT_FDCWD, path,
                            NATIVE_O_WRONLY | NATIVE_O_CREAT | NATIVE_O_TRUNC, 0644);
    if (fd < 0)
        return BLYT_ERR_IO;

    blys_buffer_t views[NATIVE_MAX_BUF];
    void *fd_ptrs[NATIVE_MAX_BUF][NATIVE_MAX_FIELDS];
    fill_native_views(views, fd_ptrs);
    blys_header_t hdr = {.schema_hash = s_schema_hash, .save_version = s_save_version};

    int rc = blys_write(&hdr, views, s_n_buffers, native_sink, &fd);
    if (rc != BLYS_OK) {
        blyt_rs_close(fd);
        return BLYT_ERR_IO;
    }

    blyt_rs_fsync(fd);
    blyt_rs_close(fd);
    blyt32_trace_call("save_write", (long)slot, 1, 0);
    return BLYT_OK;
}

blyt_result_t blyt_save_read(uint32_t slot) {
    const char *save_dir = getenv("BLYT_SAVE_DIR");
    if (!save_dir || save_dir[0] == '\0')
        return BLYT_ERR_IO;

    char path[512];
    build_save_path(path, sizeof(path), save_dir, slot);

    int fd = blyt_rs_openat(NATIVE_AT_FDCWD, path, NATIVE_O_RDONLY, 0);
    if (fd < 0)
        return BLYT_ERR_NOT_FOUND;

    blys_buffer_t views[NATIVE_MAX_BUF];
    void *fd_ptrs[NATIVE_MAX_BUF][NATIVE_MAX_FIELDS];
    fill_native_views(views, fd_ptrs);

    uint32_t saved_version = 0;
    int rc = blys_read(s_schema_hash, views, s_n_buffers, native_source, &fd, &saved_version);
    blyt_rs_close(fd);
    if (rc == BLYS_ERR_SCHEMA)
        return BLYT_ERR_SCHEMA_MISMATCH;
    if (rc != BLYS_OK)
        return BLYT_ERR_IO;

    blyt32_trace_call("save_read", (long)slot, 1, 0);

    /* Notify the cart that state was loaded, reporting the version that wrote
     * the save (ADR-0087/0125). */
    blyt_load_info_t info;
    blyt32_native_memset(&info, 0, sizeof(info));
    info.reason = BLYT_LOAD_SAVE_GAME;
    info.saved_cart_version = saved_version;
    blyt_cart_on_load_state(info);

    return BLYT_OK;
}
