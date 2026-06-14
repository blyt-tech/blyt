/*
 * libblyt32 — Blyt32 variant shared library, emulated path.
 *
 * Provides ECALL stubs for all Blyt32 API functions.  These shadow
 * libblytcommon.so's definitions via ELF symbol resolution order (carts
 * DT_NEED libblyt32.so directly, so it loads before libblytcommon.so).
 *
 * The native-path implementation lives in frontends/native/src/libblyt32/
 * and uses real Linux syscalls instead of ECALL stubs.
 */

#include "blyt.h"

/* -------------------------------------------------------------------------
 * ECALL numbers (must match runtime/host/src/libblyt/ecall.h)
 * ------------------------------------------------------------------------- */

#define ECALL_CONSOLE_DEBUG 1
#define ECALL_SAVE_WRITE 11
#define ECALL_SAVE_READ 12
#define ECALL_BUF_OP 50

/* BUF_OP sub-opcodes */
#define BUF_OP_GET_F32 1
#define BUF_OP_SET_F32 2
#define BUF_OP_GET_I32 3
#define BUF_OP_SET_I32 4
#define BUF_OP_GET_U32 5
#define BUF_OP_SET_U32 6
#define BUF_OP_GET_I16 7
#define BUF_OP_SET_I16 8
#define BUF_OP_GET_U16 9
#define BUF_OP_SET_U16 10
#define BUF_OP_GET_I8 11
#define BUF_OP_SET_I8 12
#define BUF_OP_GET_U8 13
#define BUF_OP_SET_U8 14
#define BUF_OP_GET_BOOL 15
#define BUF_OP_SET_BOOL 16
#define BUF_OP_ALLOC_SLOT 17
#define BUF_OP_FREE_SLOT 18
#define BUF_OP_REF 19
#define BUF_OP_REF_VALID 20
#define BUF_OP_GET_F64 21 /* Spike U */
#define BUF_OP_SET_F64 22 /* Spike U */

/* -------------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------------- */

static unsigned int blyt32_strlen(const char *s) {
    const char *p = s;
    while (*p)
        p++;
    return (unsigned int)(p - s);
}

/* Macro for a typed GET stub: issues ECALL_BUF_OP with the given sub-opcode
 * and returns the result value reinterpreted as the target type. */
#define STUB_GET(rettype, fn_suffix, op)                                                           \
    rettype blyt_buffer_get_##fn_suffix(blyt_buffer_h buf, int32_t slot, blyt_field_h field) {     \
        register long a0 __asm__("a0") = (op);                                                     \
        register long a1 __asm__("a1") = (long)(buf);                                              \
        register long a2 __asm__("a2") = (long)(slot);                                             \
        register long a3 __asm__("a3") = (long)(field);                                            \
        register long a7 __asm__("a7") = ECALL_BUF_OP;                                             \
        __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a3), "r"(a7) : "memory");      \
        long _tmp = a0;                                                                            \
        rettype _r;                                                                                \
        __builtin_memcpy(&_r, &_tmp, sizeof(_r));                                                  \
        return _r;                                                                                 \
    }

#define STUB_SET(valtype, fn_suffix, op)                                                           \
    void blyt_buffer_set_##fn_suffix(blyt_buffer_h buf, int32_t slot, blyt_field_h field,          \
                                     valtype v) {                                                  \
        unsigned int _vbits = 0;                                                                   \
        __builtin_memcpy(&_vbits, &v, sizeof(v));                                                  \
        register long a0 __asm__("a0") = (op);                                                     \
        register long a1 __asm__("a1") = (long)(buf);                                              \
        register long a2 __asm__("a2") = (long)(slot);                                             \
        register long a3 __asm__("a3") = (long)(field);                                            \
        register long a4 __asm__("a4") = (long)(_vbits);                                           \
        register long a7 __asm__("a7") = ECALL_BUF_OP;                                             \
        __asm__ volatile("ecall"                                                                   \
                         : "+r"(a0)                                                                \
                         : "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a7)                             \
                         : "memory");                                                              \
    }

/* -------------------------------------------------------------------------
 * blyt_console_debug — ADR-0085 ECALL stub (a0=ptr, a1=len)
 * ------------------------------------------------------------------------- */

void blyt_console_debug(const char *s) {
    register const char *a0 __asm__("a0") = s;
    register unsigned int a1 __asm__("a1") = blyt32_strlen(s);
    register long a7 __asm__("a7") = ECALL_CONSOLE_DEBUG;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a7) : "memory");
}

/* -------------------------------------------------------------------------
 * State buffer typed get/set stubs (ADR-0009, ADR-0010, ADR-0057)
 * ------------------------------------------------------------------------- */

STUB_GET(float, f32, BUF_OP_GET_F32)
STUB_SET(float, f32, BUF_OP_SET_F32)
STUB_GET(int32_t, i32, BUF_OP_GET_I32)
STUB_SET(int32_t, i32, BUF_OP_SET_I32)
STUB_GET(uint32_t, u32, BUF_OP_GET_U32)
STUB_SET(uint32_t, u32, BUF_OP_SET_U32)
STUB_GET(int16_t, i16, BUF_OP_GET_I16)
STUB_SET(int16_t, i16, BUF_OP_SET_I16)
STUB_GET(uint16_t, u16, BUF_OP_GET_U16)
STUB_SET(uint16_t, u16, BUF_OP_SET_U16)
STUB_GET(int8_t, i8, BUF_OP_GET_I8)
STUB_SET(int8_t, i8, BUF_OP_SET_I8)
STUB_GET(uint8_t, u8, BUF_OP_GET_U8)
STUB_SET(uint8_t, u8, BUF_OP_SET_U8)

/* f64 GET/SET are special-cased: the 64-bit value spans two registers
 * (Spike U). SET: a4=lo, a5=hi. GET: returns a0=lo, a1=hi. */
double blyt_buffer_get_f64(blyt_buffer_h buf, int32_t slot, blyt_field_h field) {
    register long a0 __asm__("a0") = BUF_OP_GET_F64;
    register long a1 __asm__("a1") = (long)(buf);
    register long a2 __asm__("a2") = (long)(slot);
    register long a3 __asm__("a3") = (long)(field);
    register long a7 __asm__("a7") = ECALL_BUF_OP;
    __asm__ volatile("ecall" : "+r"(a0), "+r"(a1) : "r"(a2), "r"(a3), "r"(a7) : "memory");
    uint64_t bits = ((uint64_t)(uint32_t)a1 << 32) | (uint32_t)a0;
    double r;
    __builtin_memcpy(&r, &bits, sizeof(r));
    return r;
}

void blyt_buffer_set_f64(blyt_buffer_h buf, int32_t slot, blyt_field_h field, double v) {
    uint64_t bits;
    __builtin_memcpy(&bits, &v, sizeof(v));
    register long a0 __asm__("a0") = BUF_OP_SET_F64;
    register long a1 __asm__("a1") = (long)(buf);
    register long a2 __asm__("a2") = (long)(slot);
    register long a3 __asm__("a3") = (long)(field);
    register long a4 __asm__("a4") = (long)(uint32_t)bits;         /* lo */
    register long a5 __asm__("a5") = (long)(uint32_t)(bits >> 32); /* hi */
    register long a7 __asm__("a7") = ECALL_BUF_OP;
    __asm__ volatile("ecall"
                     : "+r"(a0)
                     : "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a7)
                     : "memory");
}

/* bool GET/SET are special-cased because bool is not a fixed-size integer. */
bool blyt_buffer_get_bool(blyt_buffer_h buf, int32_t slot, blyt_field_h field) {
    register long a0 __asm__("a0") = BUF_OP_GET_BOOL;
    register long a1 __asm__("a1") = (long)(buf);
    register long a2 __asm__("a2") = (long)(slot);
    register long a3 __asm__("a3") = (long)(field);
    register long a7 __asm__("a7") = ECALL_BUF_OP;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a3), "r"(a7) : "memory");
    return a0 != 0;
}

void blyt_buffer_set_bool(blyt_buffer_h buf, int32_t slot, blyt_field_h field, bool v) {
    register long a0 __asm__("a0") = BUF_OP_SET_BOOL;
    register long a1 __asm__("a1") = (long)(buf);
    register long a2 __asm__("a2") = (long)(slot);
    register long a3 __asm__("a3") = (long)(field);
    register long a4 __asm__("a4") = v ? 1L : 0L;
    register long a7 __asm__("a7") = ECALL_BUF_OP;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a7) : "memory");
}

/* -------------------------------------------------------------------------
 * Slot management (ADR-0058)
 * ------------------------------------------------------------------------- */

blyt_result_t blyt_buffer_alloc_slot(blyt_buffer_h buf, int32_t *out_slot) {
    /* Pass the out_slot pointer in a2; host writes the slot value there. */
    register long a0 __asm__("a0") = BUF_OP_ALLOC_SLOT;
    register long a1 __asm__("a1") = (long)(buf);
    register long a2 __asm__("a2") = (long)(out_slot);
    register long a7 __asm__("a7") = ECALL_BUF_OP;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return (blyt_result_t)a0;
}

blyt_result_t blyt_buffer_free_slot(blyt_buffer_h buf, int32_t slot) {
    register long a0 __asm__("a0") = BUF_OP_FREE_SLOT;
    register long a1 __asm__("a1") = (long)(buf);
    register long a2 __asm__("a2") = (long)(slot);
    register long a7 __asm__("a7") = ECALL_BUF_OP;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return (blyt_result_t)a0;
}

/* Packed entity refs (ADR-0096).  blyt_buffer_ref_slot is a static inline in
 * blyt.h — pure bit math, no ECALL needed. */

blyt_entity_ref_t blyt_buffer_ref(blyt_buffer_h buf, int32_t slot) {
    register long a0 __asm__("a0") = BUF_OP_REF;
    register long a1 __asm__("a1") = (long)(buf);
    register long a2 __asm__("a2") = (long)(slot);
    register long a7 __asm__("a7") = ECALL_BUF_OP;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return (blyt_entity_ref_t)a0;
}

bool blyt_buffer_ref_valid(blyt_buffer_h buf, blyt_entity_ref_t ref) {
    register long a0 __asm__("a0") = BUF_OP_REF_VALID;
    register long a1 __asm__("a1") = (long)(buf);
    register long a2 __asm__("a2") = (long)(ref);
    register long a7 __asm__("a7") = ECALL_BUF_OP;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return a0 != 0;
}

/* -------------------------------------------------------------------------
 * Save/load (ADR-0087, ADR-0125)
 *
 * blyt_save_write: the cart is responsible for flushing live state to buffers
 * before calling us; we fire on_save_state first so third-party integrations
 * get their chance, then issue the ECALL to serialize to disk.
 *
 * blyt_save_read: the ECALL restores state buffers from disk, then we call
 * on_load_state with a minimal info struct (buffers=NULL for Phase 9).
 * ------------------------------------------------------------------------- */

blyt_result_t blyt_save_write(uint32_t slot) {
    blyt_cart_on_save_state();
    register long a0 __asm__("a0") = (long)(slot);
    register long a7 __asm__("a7") = ECALL_SAVE_WRITE;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a7) : "memory");
    return (blyt_result_t)a0;
}

blyt_result_t blyt_save_read(uint32_t slot) {
    register long a0 __asm__("a0") = (long)(slot);
    register long a7 __asm__("a7") = ECALL_SAVE_READ;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a7) : "memory");
    blyt_result_t result = (blyt_result_t)a0;
    if (result == BLYT_OK) {
        blyt_load_info_t info;
        info.reason = BLYT_LOAD_SAVE_GAME;
        info.saved_cart_version = 0;
        info.buffers = 0;
        blyt_cart_on_load_state(info);
    }
    return result;
}

/* -------------------------------------------------------------------------
 * blyt_exit — clean process exit.
 *
 * Called by _blyt_entry after blyt_main() returns.  On the emulated path the
 * emulator intercepts exit_group(0) via ECALL and halts the simulation.  On
 * the native path this function is shadowed by frontends/native/src/libblyt32
 * which calls exit_group directly without going through musl's cleanup.
 * ------------------------------------------------------------------------- */

__attribute__((noreturn)) void blyt_exit(int code) {
    register long a0 __asm__("a0") = code;
    register long a7 __asm__("a7") = 94; /* SYS_exit_group */
    __asm__ volatile("ecall" : : "r"(a0), "r"(a7));
    __builtin_unreachable();
}
