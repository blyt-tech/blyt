/*
 * libblyt32 — Blyt32 variant shared library, native RISC-V path.
 *
 * Built from frontends/native/src/libblyt32/ and installed alongside the
 * LP64 launcher.  The launcher sets LD_LIBRARY_PATH so the musl ILP32 ld.so
 * finds this library before the emulated-path version.
 *
 * Provides:
 *   1. Strong definitions of blyt API functions that use real Linux syscalls,
 *      shadowing libblytcommon.so's ECALL stubs via ELF load-order precedence.
 *   2. A constructor that installs the restricted seccomp filter before any
 *      cart code runs.  The constructor fires only on the native path —
 *      blyt's custom dynlinker on emulated targets does not call ELF
 *      constructors.
 *   3. State buffer storage (Phase 9): a static SOA array backed by system
 *      musl malloc for slot management.  Save/load uses raw file syscalls.
 *
 * C library (malloc, getenv, etc.) comes from ld-blyt.so.1 (system musl) via
 * the DT_NEEDED chain: libblyt32.so → libblytc.so → ld-blyt.so.1.
 */

#include "blyt.h"
#include "seccomp_restricted.h"

/* ── System musl function declarations ──────────────────────────────────────
 *
 * These symbols are provided at runtime via ld-blyt.so.1 (system musl).
 * Declared here to give the compiler proper type information without needing
 * to include the full musl header tree at build time.
 */
#include <stddef.h>
#include <stdint.h>
extern char *getenv(const char *name);

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

/* ── BLYT_TRACE api channel (native path) ────────────────────────────────
 *
 * Mirrors the host trace module's line format ("[blyt:api] <msg>") minus the
 * frame/time counters — runtime/host trace code cannot run inside the
 * seccomp'd ILP32 cart process.  getenv() comes from ld-blyt.so.1; all
 * formatting is hand-rolled (no snprintf in this library) and emission is a
 * raw SYS_write to fd 2, both already used in this file and present in the
 * restricted seccomp allowlist.  f32 values are printed as raw hex bits.
 */

static void blyt32_trace_write(const char *buf, unsigned int len) {
    register long a0 __asm__("a0") = 2; /* STDERR_FILENO */
    register const char *a1 __asm__("a1") = buf;
    register long a2 __asm__("a2") = (long)len;
    register long a7 __asm__("a7") = 64; /* SYS_write */
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
}

/* 1 when BLYT_TRACE contains the "api" (or "all") channel token. */
static int blyt32_trace_api_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        cached = 0;
        const char *p = getenv("BLYT_TRACE");
        while (p && *p) {
            if (((p[0] == 'a' && p[1] == 'p' && p[2] == 'i') ||
                 (p[0] == 'a' && p[1] == 'l' && p[2] == 'l')) &&
                (p[3] == '\0' || p[3] == ',')) {
                cached = 1;
                break;
            }
            while (*p && *p != ',')
                p++;
            if (*p)
                p++;
        }
    }
    return cached;
}

static char *blyt32_trace_app_str(char *p, char *end, const char *s) {
    while (*s && p + 1 < end)
        *p++ = *s++;
    return p;
}

static char *blyt32_trace_app_dec(char *p, char *end, long v) {
    char num[16];
    int ni = 0;
    unsigned long u = (v < 0) ? (unsigned long)-v : (unsigned long)v;
    if (v < 0 && p + 1 < end)
        *p++ = '-';
    do {
        num[ni++] = (char)('0' + (int)(u % 10u));
        u /= 10u;
    } while (u && ni < (int)sizeof(num));
    for (int j = ni - 1; j >= 0 && p + 1 < end; j--)
        *p++ = num[j];
    return p;
}

static char *blyt32_trace_app_hex(char *p, char *end, uint32_t v) {
    p = blyt32_trace_app_str(p, end, "0x");
    int started = 0;
    for (int shift = 28; shift >= 0; shift -= 4) {
        unsigned int nyb = (v >> shift) & 0xFu;
        if (!nyb && !started && shift)
            continue;
        started = 1;
        if (p + 1 < end)
            *p++ = "0123456789abcdef"[nyb];
    }
    return p;
}

static void blyt32_trace_emit(char *buf, char *p, char *end) {
    if (p + 1 < end)
        *p++ = '\n';
    blyt32_trace_write(buf, (unsigned int)(p - buf));
}

/* One line per typed buffer get/set: "[blyt:api] <name>(buf=…, slot=…,
 * field=…[, v=…])[ -> …]".  f32 bits are printed as hex. */
static void blyt32_trace_buf_op(const char *name, uint32_t b, int32_t s, uint32_t f, uint32_t bits,
                                int is_set, int is_f32) {
    if (!blyt32_trace_api_enabled())
        return;
    char buf[160];
    char *end = buf + sizeof(buf);
    char *p = blyt32_trace_app_str(buf, end, "[blyt:api] ");
    p = blyt32_trace_app_str(p, end, name);
    p = blyt32_trace_app_str(p, end, "(buf=");
    p = blyt32_trace_app_dec(p, end, (long)b);
    p = blyt32_trace_app_str(p, end, ", slot=");
    p = blyt32_trace_app_dec(p, end, (long)s);
    p = blyt32_trace_app_str(p, end, ", field=");
    p = blyt32_trace_app_dec(p, end, (long)(f & 0xFFFFu));
    p = blyt32_trace_app_str(p, end, is_set ? ", v=" : ") -> ");
    if (is_f32)
        p = blyt32_trace_app_hex(p, end, bits);
    else
        p = blyt32_trace_app_dec(p, end, (long)(int32_t)bits);
    if (is_set)
        p = blyt32_trace_app_str(p, end, ")");
    blyt32_trace_emit(buf, p, end);
}

/* Simple "<name>(arg) [-> ret]" line for the non-buffer APIs. */
static void blyt32_trace_call(const char *name, long arg, int has_ret, long ret) {
    if (!blyt32_trace_api_enabled())
        return;
    char buf[128];
    char *end = buf + sizeof(buf);
    char *p = blyt32_trace_app_str(buf, end, "[blyt:api] ");
    p = blyt32_trace_app_str(p, end, name);
    p = blyt32_trace_app_str(p, end, "(");
    p = blyt32_trace_app_dec(p, end, arg);
    p = blyt32_trace_app_str(p, end, ")");
    if (has_ret) {
        p = blyt32_trace_app_str(p, end, " -> ");
        p = blyt32_trace_app_dec(p, end, ret);
    }
    blyt32_trace_emit(buf, p, end);
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

/* ── Native state buffer SOA storage (Phase 9) ──────────────────────────── */

/* Fixed-dimension limits.  Field handles encode (buf_id-1, field_index-1);
 * slot indices are zero-based.  These must be ≥ the cart's declared values. */
#define NATIVE_MAX_BUF 8
#define NATIVE_MAX_SLOTS 64
#define NATIVE_MAX_FIELDS 64

/* SOA: one uint32_t per (buffer, slot, field).  All types stored as 32-bit
 * raw bits; narrower types (i8, u8, i16, u16, bool) use the low bits only.
 * Zero-initialized BSS: all slots start empty and all values start at 0. */
/* 64-bit per field so f64 fits (Spike U); 32-bit fields use the low word. */
static uint64_t s_soa[NATIVE_MAX_BUF][NATIVE_MAX_SLOTS][NATIVE_MAX_FIELDS];

/* Slot allocation bitset: bit i of byte (i/8) indicates slot i is allocated. */
static uint8_t s_slot_bits[NATIVE_MAX_BUF][NATIVE_MAX_SLOTS / 8];

/* Per-slot generation counters (ADR-0096), stored BIASED: the array holds
 * (generation - 1) so the BSS zero is generation 1 with no initialization
 * code (ELF constructors do not run on this path — carts enter via the
 * custom _blyt_entry, not __libc_start_main, so init arrays are skipped).
 * Public generations are 1..65535 (0 is reserved so a packed ref to slot 0
 * never equals BLYT_ENTITY_REF_NONE); stored values are 0..65534.  Bumped
 * on successful free_slot, wrapping 65535 -> 1 (stored: 65534 -> 0) — must
 * match the emulated path (state_buffer.c) exactly for determinism. */
static uint16_t s_gen[NATIVE_MAX_BUF][NATIVE_MAX_SLOTS];

/* Per-buffer declared slot count from .cart.layouts, loaded at startup.
 * Initialised to NATIVE_MAX_SLOTS in load_cart_buf_counts(); until then the
 * array is zero (BSS) but slot functions are not called before startup. */
static uint32_t s_buf_count[NATIVE_MAX_BUF];

static uint16_t native_gen(uint32_t bi, int32_t s) {
    return (uint16_t)(s_gen[bi][s] + 1u);
}

/* Save format magic and version (little-endian). */
#define NATIVE_SAVE_MAGIC_0 'N'
#define NATIVE_SAVE_MAGIC_1 'L'
#define NATIVE_SAVE_MAGIC_2 'B'
#define NATIVE_SAVE_MAGIC_3 'Y'
#define NATIVE_SAVE_VERSION 1u

/* Canonical NaN for f32 writes (ADR-0010). */
#define F32_CANONICAL_NAN 0x7FC00000u

static uint32_t canon_f32(uint32_t bits) {
    if ((bits & 0x7F800000u) == 0x7F800000u && (bits & 0x007FFFFFu) != 0u)
        return F32_CANONICAL_NAN;
    return bits;
}

/* f64 NaN canonicalisation (ADR-0010), Spike U. */
#define F64_CANONICAL_NAN 0x7FF8000000000000ULL
static uint64_t canon_f64(uint64_t bits) {
    if ((bits & 0x7FF0000000000000ULL) == 0x7FF0000000000000ULL &&
        (bits & 0x000FFFFFFFFFFFFFULL) != 0u)
        return F64_CANONICAL_NAN;
    return bits;
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
 * counts into s_buf_count[].  Must be called before blyt_install_restricted_filter():
 * uses openat/statx/mmap/munmap which are in the launcher's RV32 filter but not
 * in the restricted filter.  On any error the NATIVE_MAX_SLOTS defaults stand.
 *
 * FlatBuffer layout assumed (schemas/cart_layouts.fbs):
 *   CartLayouts: field 1 (buffers) at vtable offset 6
 *   BufferDecl:  field 2 (count)   at vtable offset 8
 */
static void load_cart_buf_counts(void) {
    /* Safe defaults until/unless the ELF is parsed successfully. */
    for (uint32_t bi = 0; bi < NATIVE_MAX_BUF; bi++)
        s_buf_count[bi] = NATIVE_MAX_SLOTS;

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

    /* ── Inline ELF32 parse (no elf.h; offsets from the ELF spec) ── */
    /* ELF32 header: e_shoff at 32 (u32), e_shnum at 48 (u16), e_shstrndx at 50 (u16). */
    uint32_t e_shoff;
    uint16_t e_shnum, e_shstrndx;
    blyt32_native_memcpy(&e_shoff, m + 32, 4);
    blyt32_native_memcpy(&e_shnum, m + 48, 2);
    blyt32_native_memcpy(&e_shstrndx, m + 50, 2);
    /* Each Elf32_Shdr is 40 bytes. */
    if (e_shoff == 0 || e_shnum == 0 || e_shstrndx >= e_shnum ||
        (uint64_t)e_shoff + (uint64_t)e_shnum * 40u > file_size)
        goto done;

    const uint8_t *shdrs = m + e_shoff;

    /* String table: sh_offset at Shdr+16 (u32), sh_size at Shdr+20 (u32). */
    const uint8_t *strtab_shdr = shdrs + (uint32_t)e_shstrndx * 40u;
    uint32_t strtab_off, strtab_size;
    blyt32_native_memcpy(&strtab_off, strtab_shdr + 16, 4);
    blyt32_native_memcpy(&strtab_size, strtab_shdr + 20, 4);
    if (strtab_off == 0 || strtab_size == 0 || (uint64_t)strtab_off + strtab_size > file_size)
        goto done;
    const char *strtab = (const char *)(m + strtab_off);

    /* Find .cart.layouts section. */
    static const char kLayouts[] = ".cart.layouts";
    uint32_t layouts_off = 0, layouts_size = 0;
    for (uint16_t si = 0; si < e_shnum; si++) {
        const uint8_t *shdr = shdrs + (uint32_t)si * 40u;
        uint32_t sh_name;
        blyt32_native_memcpy(&sh_name, shdr, 4);
        if (sh_name >= strtab_size)
            continue;
        const char *sname = strtab + sh_name;
        int match = 1;
        for (unsigned int ci = 0; ci < sizeof(kLayouts); ci++) {
            if (sname[ci] != kLayouts[ci]) {
                match = 0;
                break;
            }
        }
        if (!match)
            continue;
        blyt32_native_memcpy(&layouts_off, shdr + 16, 4);
        blyt32_native_memcpy(&layouts_size, shdr + 20, 4);
        break;
    }
    if (layouts_off == 0 || layouts_size <= 8 || (uint64_t)layouts_off + layouts_size > file_size)
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
    }

done:;
    register long a0 __asm__("a0") = (long)map;
    register long a1 __asm__("a1") = file_size;
    register long a7 __asm__("a7") = 215; /* SYS_munmap */
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a7) : "memory");
}

void blyt_runtime_startup(void) {
    load_cart_buf_counts(); /* must precede restricted filter (uses statx/mmap) */
    if (blyt_install_restricted_filter() != 0) {
        static const char msg[] = "libblyt32: FATAL: seccomp install failed\n";
        blyt_rs_write(2, msg, sizeof(msg) - 1);
        blyt_rs_exit_group(127);
    }
    /* Explicitly reset FCSR to RNE+no-flags regardless of OS/ld.so state. */
    __asm__ volatile("csrw fcsr, zero" ::: "memory");
}

/* ── Native API implementations ──────────────────────────────────────────
 *
 * Strong definitions shadow libblytcommon.so's ECALL stubs: libblyt32.so is
 * in the cart's direct DT_NEEDED so it loads and wins symbol resolution
 * before libblytcommon.so.
 */

/* blyt_frame_done — frame-boundary housekeeping on the native path.
 *
 * Called by the cart at the end of each logical frame.  Enforces FP
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
    if (blyt32_trace_api_enabled()) {
        static const char msg[] = "[blyt:api] frame_done()\n";
        blyt32_trace_write(msg, sizeof(msg) - 1);
    }
}

/* blyt_exit — clean process exit after cart main loop.
 *
 * Called by _blyt_entry (the ELF entry point stub) after blyt_main() returns.
 * Bypasses musl's exit() cleanup path, which calls munmap and other syscalls
 * blocked by the restricted seccomp filter.  exit_group(0) is in the allowlist.
 *
 * Declared __attribute__((noreturn)) so the compiler can omit the return path
 * in _blyt_entry and avoid generating a dead-code epilogue.
 */
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

/* ── State buffer typed get/set (Phase 9) ────────────────────────────────
 *
 * Buffer handle (buf_h): 1-based buffer index.
 * Field handle (field_h): upper 16 bits = buf_id (must match buf_h),
 *                          lower 16 bits = 1-based field index.
 * All values stored as raw uint32_t bits in s_soa[][slot][field_index].
 * Narrower types use the low bits of the 32-bit slot. */

float blyt_buffer_get_f32(blyt_buffer_h b, int32_t s, blyt_field_h f) {
    uint32_t bi = b - 1u, fi = (f & 0xFFFFu) - 1u;
    if (!ref_ok(bi, s, fi))
        return 0.0f;
    uint32_t bits = s_soa[bi][s][fi];
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
    s_soa[bi][s][fi] = canon_f32(bits);
}

/* f64 (Spike U): full 64-bit field; stored in the widened SOA slot. */
double blyt_buffer_get_f64(blyt_buffer_h b, int32_t s, blyt_field_h f) {
    uint32_t bi = b - 1u, fi = (f & 0xFFFFu) - 1u;
    if (!ref_ok(bi, s, fi))
        return 0.0;
    uint64_t bits = s_soa[bi][s][fi];
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
    s_soa[bi][s][fi] = canon_f64(bits);
}

int32_t blyt_buffer_get_i32(blyt_buffer_h b, int32_t s, blyt_field_h f) {
    uint32_t bi = b - 1u, fi = (f & 0xFFFFu) - 1u;
    if (!ref_ok(bi, s, fi))
        return 0;
    uint32_t bits = s_soa[bi][s][fi];
    blyt32_trace_buf_op("buf_get_i32", b, s, f, bits, 0, 0);
    return (int32_t)bits;
}
void blyt_buffer_set_i32(blyt_buffer_h b, int32_t s, blyt_field_h f, int32_t v) {
    uint32_t bi = b - 1u, fi = (f & 0xFFFFu) - 1u;
    if (!ref_ok(bi, s, fi))
        return;
    blyt32_trace_buf_op("buf_set_i32", b, s, f, (uint32_t)v, 1, 0);
    s_soa[bi][s][fi] = (uint32_t)v;
}

uint32_t blyt_buffer_get_u32(blyt_buffer_h b, int32_t s, blyt_field_h f) {
    uint32_t bi = b - 1u, fi = (f & 0xFFFFu) - 1u;
    if (!ref_ok(bi, s, fi))
        return 0u;
    uint32_t bits = s_soa[bi][s][fi];
    blyt32_trace_buf_op("buf_get_u32", b, s, f, bits, 0, 0);
    return bits;
}
void blyt_buffer_set_u32(blyt_buffer_h b, int32_t s, blyt_field_h f, uint32_t v) {
    uint32_t bi = b - 1u, fi = (f & 0xFFFFu) - 1u;
    if (!ref_ok(bi, s, fi))
        return;
    blyt32_trace_buf_op("buf_set_u32", b, s, f, v, 1, 0);
    s_soa[bi][s][fi] = v;
}

int16_t blyt_buffer_get_i16(blyt_buffer_h b, int32_t s, blyt_field_h f) {
    uint32_t bi = b - 1u, fi = (f & 0xFFFFu) - 1u;
    if (!ref_ok(bi, s, fi))
        return 0;
    int16_t v16 = (int16_t)(s_soa[bi][s][fi] & 0xFFFFu);
    blyt32_trace_buf_op("buf_get_i16", b, s, f, (uint32_t)(int32_t)v16, 0, 0);
    return v16;
}
void blyt_buffer_set_i16(blyt_buffer_h b, int32_t s, blyt_field_h f, int16_t v) {
    uint32_t bi = b - 1u, fi = (f & 0xFFFFu) - 1u;
    if (!ref_ok(bi, s, fi))
        return;
    blyt32_trace_buf_op("buf_set_i16", b, s, f, (uint32_t)(int32_t)v, 1, 0);
    s_soa[bi][s][fi] = (uint32_t)(uint16_t)v;
}

uint16_t blyt_buffer_get_u16(blyt_buffer_h b, int32_t s, blyt_field_h f) {
    uint32_t bi = b - 1u, fi = (f & 0xFFFFu) - 1u;
    if (!ref_ok(bi, s, fi))
        return 0u;
    uint16_t v16 = (uint16_t)(s_soa[bi][s][fi] & 0xFFFFu);
    blyt32_trace_buf_op("buf_get_u16", b, s, f, (uint32_t)v16, 0, 0);
    return v16;
}
void blyt_buffer_set_u16(blyt_buffer_h b, int32_t s, blyt_field_h f, uint16_t v) {
    uint32_t bi = b - 1u, fi = (f & 0xFFFFu) - 1u;
    if (!ref_ok(bi, s, fi))
        return;
    blyt32_trace_buf_op("buf_set_u16", b, s, f, (uint32_t)v, 1, 0);
    s_soa[bi][s][fi] = (uint32_t)v;
}

int8_t blyt_buffer_get_i8(blyt_buffer_h b, int32_t s, blyt_field_h f) {
    uint32_t bi = b - 1u, fi = (f & 0xFFFFu) - 1u;
    if (!ref_ok(bi, s, fi))
        return 0;
    int8_t v8 = (int8_t)(s_soa[bi][s][fi] & 0xFFu);
    blyt32_trace_buf_op("buf_get_i8", b, s, f, (uint32_t)(int32_t)v8, 0, 0);
    return v8;
}
void blyt_buffer_set_i8(blyt_buffer_h b, int32_t s, blyt_field_h f, int8_t v) {
    uint32_t bi = b - 1u, fi = (f & 0xFFFFu) - 1u;
    if (!ref_ok(bi, s, fi))
        return;
    blyt32_trace_buf_op("buf_set_i8", b, s, f, (uint32_t)(int32_t)v, 1, 0);
    s_soa[bi][s][fi] = (uint32_t)(uint8_t)v;
}

uint8_t blyt_buffer_get_u8(blyt_buffer_h b, int32_t s, blyt_field_h f) {
    uint32_t bi = b - 1u, fi = (f & 0xFFFFu) - 1u;
    if (!ref_ok(bi, s, fi))
        return 0u;
    uint8_t v8 = (uint8_t)(s_soa[bi][s][fi] & 0xFFu);
    blyt32_trace_buf_op("buf_get_u8", b, s, f, (uint32_t)v8, 0, 0);
    return v8;
}
void blyt_buffer_set_u8(blyt_buffer_h b, int32_t s, blyt_field_h f, uint8_t v) {
    uint32_t bi = b - 1u, fi = (f & 0xFFFFu) - 1u;
    if (!ref_ok(bi, s, fi))
        return;
    blyt32_trace_buf_op("buf_set_u8", b, s, f, (uint32_t)v, 1, 0);
    s_soa[bi][s][fi] = (uint32_t)v;
}

bool blyt_buffer_get_bool(blyt_buffer_h b, int32_t s, blyt_field_h f) {
    uint32_t bi = b - 1u, fi = (f & 0xFFFFu) - 1u;
    if (!ref_ok(bi, s, fi))
        return false;
    bool vb = s_soa[bi][s][fi] != 0u;
    blyt32_trace_buf_op("buf_get_bool", b, s, f, vb ? 1u : 0u, 0, 0);
    return vb;
}
void blyt_buffer_set_bool(blyt_buffer_h b, int32_t s, blyt_field_h f, bool v) {
    uint32_t bi = b - 1u, fi = (f & 0xFFFFu) - 1u;
    if (!ref_ok(bi, s, fi))
        return;
    blyt32_trace_buf_op("buf_set_bool", b, s, f, v ? 1u : 0u, 1, 0);
    s_soa[bi][s][fi] = v ? 1u : 0u;
}

/* ── Slot management ──────────────────────────────────────────────────────��� */

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
    /* Zero the freed slot's field data (the emulated path does). */
    for (uint32_t fi = 0; fi < NATIVE_MAX_FIELDS; fi++)
        s_soa[bi][s][fi] = 0;
    /* Bump the generation, wrapping 65535 -> 1 (ADR-0096); stored values are
     * biased by -1, so the stored wrap is 65534 -> 0. */
    s_gen[bi][s] = (uint16_t)(s_gen[bi][s] >= 0xFFFEu ? 0 : s_gen[bi][s] + 1);
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

/* ── Save / load (Phase 9) ────────────────────────────────────────────────
 *
 * Save file format (all values little-endian):
 *   [0..3]    magic: 'N','L','B','Y'
 *   [4..7]    version: 1u (uint32)
 *   [8..N]    raw s_soa array
 *   [N..N+B]  raw s_slot_bits array
 *   [..]      raw s_gen array (per-slot generation counters, ADR-0096;
 *             stored biased by -1, see the s_gen declaration)
 *
 * BLYT_SAVE_DIR environment variable (set by test runner) determines the
 * directory.  File name: slot_<N>.blys in that directory.
 */

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

    /* Write header: magic + version (8 bytes) */
    uint8_t hdr[8];
    hdr[0] = NATIVE_SAVE_MAGIC_0;
    hdr[1] = NATIVE_SAVE_MAGIC_1;
    hdr[2] = NATIVE_SAVE_MAGIC_2;
    hdr[3] = NATIVE_SAVE_MAGIC_3;
    hdr[4] = (uint8_t)(NATIVE_SAVE_VERSION & 0xFFu);
    hdr[5] = (uint8_t)((NATIVE_SAVE_VERSION >> 8) & 0xFFu);
    hdr[6] = (uint8_t)((NATIVE_SAVE_VERSION >> 16) & 0xFFu);
    hdr[7] = (uint8_t)((NATIVE_SAVE_VERSION >> 24) & 0xFFu);
    if (write_all(fd, hdr, 8) < 0) {
        blyt_rs_close(fd);
        return BLYT_ERR_IO;
    }

    /* Write SOA data */
    if (write_all(fd, s_soa, sizeof(s_soa)) < 0) {
        blyt_rs_close(fd);
        return BLYT_ERR_IO;
    }

    /* Write slot bitsets */
    if (write_all(fd, s_slot_bits, sizeof(s_slot_bits)) < 0) {
        blyt_rs_close(fd);
        return BLYT_ERR_IO;
    }

    /* Write generation counters (ADR-0096) */
    if (write_all(fd, s_gen, sizeof(s_gen)) < 0) {
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

    /* Read and verify header */
    uint8_t hdr[8];
    if (read_all(fd, hdr, 8) != 0) {
        blyt_rs_close(fd);
        return BLYT_ERR_IO;
    }

    if (hdr[0] != NATIVE_SAVE_MAGIC_0 || hdr[1] != NATIVE_SAVE_MAGIC_1 ||
        hdr[2] != NATIVE_SAVE_MAGIC_2 || hdr[3] != NATIVE_SAVE_MAGIC_3) {
        blyt_rs_close(fd);
        return BLYT_ERR_IO;
    }

    /* Read SOA data */
    if (read_all(fd, s_soa, sizeof(s_soa)) != 0) {
        blyt_rs_close(fd);
        return BLYT_ERR_IO;
    }

    /* Read slot bitsets */
    if (read_all(fd, s_slot_bits, sizeof(s_slot_bits)) != 0) {
        blyt_rs_close(fd);
        return BLYT_ERR_IO;
    }

    /* Read generation counters (ADR-0096) */
    if (read_all(fd, s_gen, sizeof(s_gen)) != 0) {
        blyt_rs_close(fd);
        return BLYT_ERR_IO;
    }

    blyt_rs_close(fd);
    blyt32_trace_call("save_read", (long)slot, 1, 0);

    /* Notify the cart that state was loaded. */
    blyt_load_info_t info;
    blyt32_native_memset(&info, 0, sizeof(info));
    info.reason = BLYT_LOAD_SAVE_GAME;
    blyt_cart_on_load_state(info);

    return BLYT_OK;
}
