#pragma once

#include <stdint.h>

/*
 * Blyt ECALL numbers (a7 register before the ecall instruction, ADR-0085).
 *
 * Cart code never issues ecall instructions.  ECALLs are issued by
 * libblyt32.so's stub functions inside the rv32emu guest address space.
 * When rv32emu's on_ecall fires, the runtime dispatches on a7.
 *
 * Number space (ADR-0085):
 *   0        exit/abort — halt emulation; a0=0 clean, a0≠0 abort
 *   1–49     lifecycle (console_debug, frame_done, …)
 *   100–199  graphics
 *   …
 *
 * BLYT_TRACE convention: every ECALL case in blyt_ecall_handler() (and every
 * bridge sub-op in bridge_lua_op()) emits one typed blyt_tracef(BLYT_TRACE_API,
 * …) line using the cart-facing public name and the most useful formatting for
 * that API (ints as ints, slots as slots, strings dereferenced).  When adding
 * a new ECALL number here, add its trace line in the handler too — only the
 * default/unknown path falls back to a generic hex dump.
 */
#define BLYT_ECALL_EXIT 0 /* halt emulation; a0=exit code (0=clean, 1=abort) */
#define BLYT_ECALL_CONSOLE_DEBUG 1 /* blyt_console_debug: a0=ptr, a1=len */
#define BLYT_ECALL_FRAME_DONE 2 /* end of one update+draw cycle */
/* DAP hook: a0=source_vaddr, a1=source_len, a2=line, a3=depth
 * returns 1 (guest should pause) or 0 (continue); host sends "stopped" event */
#define BLYT_ECALL_DAP_HOOK 3
/* DAP send: a0=json_vaddr, a1=json_len; forwards JSON to DAP TCP client */
#define BLYT_ECALL_DAP_SEND 4
/* DAP recv: a0=buf_vaddr, a1=max_len; blocks until inspection command arrives;
 * writes JSON to guest buf; returns length in a0 (0 = continue/disconnect) */
#define BLYT_ECALL_DAP_RECV 5

/* ECALLs 6–8 are reserved for DAP extension operations (handled as literal
 * case labels in the ecall dispatch; see cart_run.c for details). */

/* Host-function return: fired by BLYT_TRAMPOLINE_FN_RETURN_ADDR when a
 * guest function invoked via blyt_session_begin_fn_call() returns.
 * The return value is already in a0 from the C function's ret instruction;
 * this ecall signals the host to stop driving rv32emu and read a0. */
#define BLYT_ECALL_HOST_FN_RETURN 9

/* Save/load ECALLs (ADR-0087, ADR-0125).
 * SAVE_WRITE: a0=slot; returns blyt_result_t in a0.
 * SAVE_READ:  a0=slot; returns blyt_result_t in a0; fires on_load_state. */
#define BLYT_ECALL_SAVE_WRITE 11
#define BLYT_ECALL_SAVE_READ 12

/* State buffer op (ADR-0009, ADR-0057, ADR-0058).
 * a0 = sub-opcode (BUF_OP_*), remaining args per sub-opcode below.
 * For GET ops: a1=buf_h, a2=slot, a3=field_h; returns value bits in a0.
 * For SET ops: a1=buf_h, a2=slot, a3=field_h, a4=value (uint32_t bit pattern).
 * For ALLOC/FREE: a1=buf_h, a2=slot (FREE) or a2=out_slot ptr (ALLOC). */
#define BLYT_ECALL_BUF_OP 50

/* Sub-opcodes for BLYT_ECALL_BUF_OP (a0).
 * type_tag encoding: 0=i8 1=u8 2=i16 3=u16 4=i32 5=u32 6=f32 7=bool */
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

/* Lua C API bridge op (ADR-0130, WASM hybrid carts only).
 * Issued by the bridge-stub variant of libblyt32lua.so while a bridged
 * Lua→native call is in flight.  a0=opcode (BLYT_LUA_OP_*), a1=call token,
 * a2–a5=op args.  Returns a0=status (BLYT_LUA_ST_*), a1=value, a2=aux.
 * Outside the bridged-call window, or with a bad token/opcode, this traps. */
#define BLYT_ECALL_LUA_OP 10

/* Bridge opcodes (a0).  One per bridged Lua C API operation; the dispatch
 * switch in lua_bridge.c is the ADR-0118 enforcement point on WASM — the
 * loading/compiling class has no opcodes by construction. */
enum {
    BLYT_LUA_OP_GETTOP = 1, /* () -> top */
    BLYT_LUA_OP_SETTOP = 2, /* (idx) */
    BLYT_LUA_OP_PUSHVALUE = 3, /* (idx) */
    BLYT_LUA_OP_TYPE = 4, /* (idx) -> LUA_T* */
    BLYT_LUA_OP_PUSHNIL = 5, /* () */
    BLYT_LUA_OP_PUSHBOOLEAN = 6, /* (b) */
    BLYT_LUA_OP_PUSHINTEGER = 7, /* (n) */
    BLYT_LUA_OP_PUSHNUMBER = 8, /* (f32 bits) */
    BLYT_LUA_OP_PUSHLSTRING = 9, /* (ptr, len) */
    BLYT_LUA_OP_TOINTEGERX = 10, /* (idx) -> n, aux=isnum */
    BLYT_LUA_OP_TONUMBERX = 11, /* (idx) -> f32 bits, aux=isnum */
    BLYT_LUA_OP_TOBOOLEAN = 12, /* (idx) -> 0/1 */
    BLYT_LUA_OP_TOLSTRING = 13, /* (idx, buf, cap) -> wrote, aux=full len */
    BLYT_LUA_OP_CREATETABLE = 14, /* (narr, nrec) */
    BLYT_LUA_OP_GETFIELD = 15, /* (idx, k_ptr, k_len) -> type */
    BLYT_LUA_OP_SETFIELD = 16, /* (idx, k_ptr, k_len) */
    BLYT_LUA_OP_GETI = 17, /* (idx, i) -> type */
    BLYT_LUA_OP_SETI = 18, /* (idx, i) */
    BLYT_LUA_OP_RAWLEN = 19, /* (idx) -> len */
    BLYT_LUA_OP_NEXT = 20, /* (idx) -> 0/1 */
    BLYT_LUA_OP_GETGLOBAL = 21, /* (name_ptr, name_len) -> type */
    BLYT_LUA_OP_SETGLOBAL = 22, /* (name_ptr, name_len) */
    BLYT_LUA_OP_ERROR = 23, /* () — never returns to the guest */
    BLYT_LUA_OP_ERRMSG = 24, /* (msg_ptr, msg_len) — never returns */
};

/* Bridge op status (returned in a0).  Lua errors never return: the host
 * halts emulation and raises from the trampoline continuation instead. */
#define BLYT_LUA_ST_OK 0
#define BLYT_LUA_ST_RETRY 1 /* TOLSTRING: cap too small; aux has needed len */
#define BLYT_LUA_ST_NIL 2 /* TOLSTRING: value not string/number (NULL return) */

/*
 * EXIT trampoline — injected into rv32emu guest memory by the runtime.
 *
 * When blyt_main returns, the CPU jumps to BLYT_TRAMPOLINE_EXIT_ADDR
 * (placed in RA before calling blyt_main).  The trampoline sets a0=0
 * (clean exit) and a7=0 (BLYT_ECALL_EXIT), then issues ecall so the
 * host can halt the emulator and report BLYT_RUN_OK.
 *
 * abort() in libblytc.so uses the same ecall but with a0=1, which the
 * handler maps to BLYT_RUN_ERR_ABORT so frontends can distinguish a
 * normal return from a fatal internal error.
 *
 * Layout at BLYT_TRAMPOLINE_BASE (16 bytes):
 *   addi x10, x0, 0   ; li a0, 0  (exit code: 0 = clean)
 *   addi x17, x0, 0   ; li a7, BLYT_ECALL_EXIT
 *   ecall
 *   unimp              ; 0x00000000 — should never be reached
 *
 * Address chosen to be above the 16 MiB cart RAM limit and below the
 * 64 MiB arena and 128 MiB library base.
 */
#define BLYT_TRAMPOLINE_BASE 0x01000000u
#define BLYT_TRAMPOLINE_EXIT_ADDR BLYT_TRAMPOLINE_BASE
/* FN_RETURN stub at +64 from exit (16 bytes each, 48 bytes gap for growth). */
#define BLYT_TRAMPOLINE_FN_RETURN_ADDR (BLYT_TRAMPOLINE_EXIT_ADDR + 64u)

#define RV32_LI_A0_0 UINT32_C(0x00000513) /* addi x10, x0, 0 */
#define RV32_LI_A7_0 UINT32_C(0x00000893) /* addi x17, x0, 0 */
#define RV32_LI_A7_9 UINT32_C(0x00900893) /* addi x17, x0, 9 (BLYT_ECALL_HOST_FN_RETURN) */
#define RV32_ECALL UINT32_C(0x00000073)
#define RV32_UNIMP UINT32_C(0x00000000)
