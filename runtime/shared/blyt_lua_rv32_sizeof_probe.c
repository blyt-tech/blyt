/*
 * blyt_lua_rv32_sizeof_probe.c — rv32 (ilp32d) object-size probe for the
 * host-Lua heap-accounting seam (BLYT_HOSTLUA_HEAP_SEAM, #231, epic #230).
 *
 * NOT compiled into any shipping binary. It is compiled ONCE, by the actual
 * rv32/wasm guest toolchain (riscv32 ilp32d), to an object file whose ELF
 * symbol table encodes each Lua internal type's canonical rv32 size: every
 * `blyt_rv32_sz__T` is a `char[sizeof(T)]`, so `nm --print-size` reads the
 * symbol's size back out as `sizeof(T)`. cmake/blyt_gen_rv32_sizeof.cmake turns
 * that into the generated header `blyt_lua_rv32_sizeof.h` which the seam bakes
 * in.
 *
 * Why generate rather than hard-code: on a 64-bit desktop host the Lua VM's
 * objects carry 8-byte pointers, so `guest_heap_used` counted at host sizes
 * diverges from the 32-bit canonical (rv32 / wasm32) the determinism contract
 * (ADR-0029) promises. The seam models the count DOWN to rv32 by substituting
 * these sizes at every allocation site. Deriving the numbers from a real rv32
 * compile makes them correct-by-construction — the runtime *is* the spec — so
 * they cannot silently rot on a Lua-fork bump (a layout change flows straight
 * through the next build's probe). DIRECTION 1 (32-bit canonical): the seam is
 * active ONLY on the 64-bit build; wasm + rv32 hardware report their real sizes
 * unchanged (this probe merely confirms what they already produce).
 *
 * Compiled with BLYT_LUA_I32_F64=1 (lua_Integer=int32, lua_Number=double) — the
 * ilp32d cart numeric model — exactly as the guest Lua libraries are, so the
 * measured layout matches the bytecode the carts actually run.
 */

#define LUA_CORE
#include "lprefix.h"

#include "lua.h"

#include "lfunc.h"
#include "llimits.h"
#include "lobject.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "lzio.h"

#include "lparser.h" /* Vardesc / Labeldesc — transient parse-time vectors */

/* One external-linkage array per type; its ELF symbol size == sizeof(type).
 * External (not static) so -O2 / -fdata-sections never elides it. */
#define BLYT_RV32_SZ(T) char blyt_rv32_sz__##T[sizeof(T)];
#define BLYT_RV32_SZ_AS(name, expr) char blyt_rv32_sz__##name[expr];

/* Non-divergent element types (TValue/StackValue/Node/Instruction/AbsLineInfo
 * are byte-identical across ilp32d and lp64 — the f64 in the Value union forces
 * 8-byte size/align on both). Emitted anyway so the seam sizes every vector it
 * touches from one generated table, not a mix of generated + assumed. */
BLYT_RV32_SZ(TValue)
BLYT_RV32_SZ(StackValue)
BLYT_RV32_SZ(Node)
BLYT_RV32_SZ(Instruction)
BLYT_RV32_SZ(AbsLineInfo)

/* Pointer-bearing GC-object headers + pure pointer arrays — the sole sources of
 * host/rv32 divergence (8-byte vs 4-byte pointers). */
BLYT_RV32_SZ(GCObject)
BLYT_RV32_SZ(TString)
BLYT_RV32_SZ(Udata)
BLYT_RV32_SZ(Udata0)
BLYT_RV32_SZ(UValue)
BLYT_RV32_SZ(Table)
BLYT_RV32_SZ(Proto)
BLYT_RV32_SZ(LClosure)
BLYT_RV32_SZ(CClosure)
BLYT_RV32_SZ(Closure)
BLYT_RV32_SZ(CallInfo)
BLYT_RV32_SZ(UpVal)
BLYT_RV32_SZ(Upvaldesc)
BLYT_RV32_SZ(LocVar)
BLYT_RV32_SZ(Vardesc)
BLYT_RV32_SZ(Labeldesc)
BLYT_RV32_SZ(lua_State)
BLYT_RV32_SZ(global_State)

/* Scalar element types (1 byte on both; present for a complete vector table). */
BLYT_RV32_SZ_AS(char, sizeof(char))
BLYT_RV32_SZ_AS(lu_byte, sizeof(lu_byte))
BLYT_RV32_SZ_AS(ls_byte, sizeof(ls_byte))

/* A generic data pointer: the rv32 size of every `T*` vector element (string
 * intern table GCObject**, Proto p[]/upvalues/locvars pointer members, …). */
BLYT_RV32_SZ_AS(ptr, sizeof(void *))

/* Thread size (LX = lua_State + luai_extraspace); the whole-object size Lua
 * accounts for a coroutine (luaC_newobjdt(LUA_TTHREAD, sizeof(LX), …)). */
BLYT_RV32_SZ_AS(LX, sizeof(LX))

/* Flexible-array-member offsets: the rv32 base offset at which each variable GC
 * object's arch-identical tail (string bytes / upvalue array / user values)
 * begins, so the seam reconstructs the exact rv32 size from the host size and
 * the recovered element count. Symbol size == the offset. */
BLYT_RV32_SZ_AS(off_TString_contents, offsetof(TString, contents))
BLYT_RV32_SZ_AS(off_TString_falloc, offsetof(TString, falloc))
BLYT_RV32_SZ_AS(off_LClosure_upvals, offsetof(LClosure, upvals))
BLYT_RV32_SZ_AS(off_CClosure_upvalue, offsetof(CClosure, upvalue))
BLYT_RV32_SZ_AS(off_Udata_uv, offsetof(Udata, uv))
