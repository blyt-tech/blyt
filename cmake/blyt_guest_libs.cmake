# cmake/blyt_guest_libs.cmake
#
# RV32IMAFC guest libraries, built by the main ninja graph DIRECTLY into the SDK
# layout (build/sdk/lib, lib/debug, lib/native, sdk/bin) so a plain `cmake
# --build build` keeps every guest artifact fresh — the sdk script
# (cmake/blyt_sdk.cmake) no longer compiles anything RV32 itself.
#
# Inputs (from cmake/blyt_rv32_toolchain.cmake): BLYT_RV32_CLANG / _LLD /
# _OBJCOPY.  When no toolchain is available every rule here is skipped, the same
# way the old inline rules were.
#
# Library set (ADR-0119/0120/0129/0130): sdk/lib/            libblytcommon,
# libblytc, libblyt32 (thick), libblytcommonlua, libblyt32lua,
# libblyt32lua-bridge — release: -O2, stripped (.dynsym kept) sdk/lib/debug/
# same set minus libblytcommonlua — -O0 -g, unstripped sdk/lib/native/
# trusted-native-exec variants for the QEMU gate sdk/bin/blyt-luac   host-native
# Lua bytecode compiler (LUA_32BITS)
#
# Aggregate targets: guest_libs (ALL), libblyt32_native_so (ALL, gated by
# BLYT_BUILD_NATIVE).

option(BLYT_BUILD_NATIVE
       "Build the native trusted-exec guest libs (RISC-V QEMU gate)" ON)

set(SDK_LIB "${CMAKE_BINARY_DIR}/sdk/lib")
set(SDK_LIB_DEBUG "${SDK_LIB}/debug")
set(SDK_LIB_NATIVE "${SDK_LIB}/native")
set(SDK_BIN "${CMAKE_BINARY_DIR}/sdk/bin")

# Release guest-lib paths consumed elsewhere in the main build (libretro embed,
# test_libretro_core gate).  Empty until the toolchain check passes.
set(LIBBLYTCOMMON_OUT "")
set(LIBBLYTC_OUT "")
set(LIBBLYT32_OUT "")
set(LIBBLYT32LUA_OUT "")

if(NOT BLYT_RV32_CLANG)
  message(STATUS "Guest libraries: skipped (no riscv32-capable clang)")
  return()
endif()

file(MAKE_DIRECTORY "${SDK_LIB}" "${SDK_LIB_DEBUG}" "${SDK_BIN}")

# ── Base flags ───────────────────────────────────────────────────────────────
set(RV32_BASE
    # riscv32-linux-gnu (not bare-metal riscv32): lld injects -static for
    # bare-metal targets which rejects .so inputs; using a Linux triple keeps
    # shared-library semantics.  -nostdlib suppresses the sysroot linkage.
    --target=riscv32-linux-gnu
    -march=rv32imafc
    -mabi=ilp32f
    -shared
    -fPIC
    -nostdlib
    # The emulated↔native split overrides libblytcommon's ECALL stubs (e.g.
    # blyt_frame_done) via symbol preemption.  -O2 defaults to
    # -fno-semantic-interposition, which would bind libblytcommon's intra-module
    # calls (blyt_main → blyt_frame_done) directly to its own stub and ecall on
    # the native path (SIGSYS under the restricted seccomp filter).  Keep
    # interposition so the native libblyt32.so override binds (ADR-0129).
    -fsemantic-interposition
    -Wl,--shared
    -I
    "${CMAKE_SOURCE_DIR}/runtime/guest/include"
    "-fuse-ld=${BLYT_RV32_LLD}")

set(GUEST_INC_DIR "${CMAKE_SOURCE_DIR}/runtime/guest/include")
set(BLYT_H "${GUEST_INC_DIR}/blyt.h")
set(BLYT32_H "${GUEST_INC_DIR}/blyt32.h")
set(BLYT_LUA_INTERNAL_H "${GUEST_INC_DIR}/blyt_lua_internal.h")

# Per-variant settings (ADR-0129): release → SDK_LIB, -O2, stripped (no DWARF,
# no .symtab; --strip-unneeded keeps .dynsym so cart-facing exports survive);
# debug → SDK_LIB/debug, -O0 -g, unstripped.  A macro so it writes to the caller
# scope.
macro(blyt_set_variant _var)
  if("${_var}" STREQUAL "debug")
    set(_VDIR "${SDK_LIB_DEBUG}")
    set(_VOPT -O0 -g)
    set(_VSTRIP FALSE)
  else()
    set(_VDIR "${SDK_LIB}")
    set(_VOPT -O2)
    set(_VSTRIP TRUE)
  endif()
endmacro()

# Declare one guest .so as a ninja rule.  ARGS = clang args after RV32_BASE
# (must include -o <out>); release variants get a strip pass appended.
function(blyt_guest_so out strip comment)
  cmake_parse_arguments(G "" "" "ARGS;DEPENDS" ${ARGN})
  set(_link COMMAND "${BLYT_RV32_CLANG}" ${RV32_BASE} ${G_ARGS})
  if(strip AND BLYT_RV32_OBJCOPY)
    list(
      APPEND
      _link
      COMMAND
      "${BLYT_RV32_OBJCOPY}"
      --strip-debug
      --strip-unneeded
      "${out}")
  endif()
  add_custom_command(
    OUTPUT "${out}" ${_link}
    DEPENDS ${G_DEPENDS}
    COMMENT "${comment}"
    VERBATIM)
endfunction()

# ── libblytcommon.so — variant-portable blyt.h ECALL stubs ──────────────────
set(LIBBLYTCOMMON_SRC
    "${CMAKE_SOURCE_DIR}/runtime/guest/src/libblytcommon/blyt_common.c")
foreach(_var release debug)
  blyt_set_variant(${_var})
  blyt_guest_so(
    "${_VDIR}/libblytcommon.so"
    ${_VSTRIP}
    "Cross-compiling libblytcommon.so (${_var})"
    ARGS
    ${_VOPT}
    -Wl,-soname,libblytcommon.so
    -o
    "${_VDIR}/libblytcommon.so"
    "${LIBBLYTCOMMON_SRC}"
    DEPENDS
    "${LIBBLYTCOMMON_SRC}"
    "${BLYT_H}")
endforeach()
set(LIBBLYTCOMMON_OUT "${SDK_LIB}/libblytcommon.so")

# ── musl bits/ generation (configure time) ──────────────────────────────────
set(MUSL_DIR "${CMAKE_SOURCE_DIR}/third_party/musl")
set(LIBBLYTC_BITS_DIR "${CMAKE_BINARY_DIR}/libblytc/bits")
set(LIBBLYTC_ALLTYPES_H "${LIBBLYTC_BITS_DIR}/alltypes.h")

if(EXISTS "${MUSL_DIR}/include/stdio.h")
  # Set up bits/: copy all arch/riscv32/bits/*.h, then generate the two that
  # need processing (alltypes.h from template; syscall.h from .in).
  file(MAKE_DIRECTORY "${LIBBLYTC_BITS_DIR}")
  file(GLOB _BITS_HDRS "${MUSL_DIR}/arch/riscv32/bits/*.h")
  foreach(_H ${_BITS_HDRS})
    file(COPY "${_H}" DESTINATION "${LIBBLYTC_BITS_DIR}")
  endforeach()

  execute_process(
    COMMAND sh -c "cat '${MUSL_DIR}/arch/riscv32/bits/alltypes.h.in' \
            '${MUSL_DIR}/include/alltypes.h.in' \
       | sed -f '${MUSL_DIR}/tools/mkalltypes.sed'"
    OUTPUT_FILE "${LIBBLYTC_ALLTYPES_H}"
    RESULT_VARIABLE _R
    ERROR_QUIET)
  if(NOT _R EQUAL 0)
    message(WARNING "libblytc.so: failed to generate bits/alltypes.h")
    set(MUSL_DIR "")
  endif()

  if(MUSL_DIR)
    execute_process(
      COMMAND
        sh -c "cp '${MUSL_DIR}/arch/riscv32/bits/syscall.h.in' \
            '${LIBBLYTC_BITS_DIR}/syscall.h' \
         && sed -n -e 's/__NR_/SYS_/p' \
             '${MUSL_DIR}/arch/riscv32/bits/syscall.h.in' \
             >> '${LIBBLYTC_BITS_DIR}/syscall.h'"
      RESULT_VARIABLE _R
      ERROR_QUIET)
    if(NOT _R EQUAL 0)
      message(WARNING "libblytc.so: failed to generate bits/syscall.h")
      set(MUSL_DIR "")
    endif()
  endif()
else()
  message(STATUS "libblytc.so: third_party/musl not initialised — skipping \
(run: git submodule update --init third_party/musl)")
  set(MUSL_DIR "")
endif()

if(NOT MUSL_DIR)
  # Without musl there is no libblytc, no thick libblyt32, no Lua libs.  Build a
  # minimal libblyt32.so (libblytcommon only) so the embed gate still works.
  foreach(_var release debug)
    blyt_set_variant(${_var})
    blyt_guest_so(
      "${_VDIR}/libblyt32.so"
      ${_VSTRIP}
      "Cross-compiling libblyt32.so (${_var}, no musl)"
      ARGS
      ${_VOPT}
      -Wl,-soname,libblyt32.so
      -Wl,-Bdynamic
      "${_VDIR}/libblytcommon.so"
      -o
      "${_VDIR}/libblyt32.so"
      "${CMAKE_SOURCE_DIR}/runtime/guest/src/libblyt32/blyt32.c"
      DEPENDS
      "${CMAKE_SOURCE_DIR}/runtime/guest/src/libblyt32/blyt32.c"
      "${BLYT_H}"
      "${_VDIR}/libblytcommon.so")
  endforeach()
  set(LIBBLYT32_OUT "${SDK_LIB}/libblyt32.so")
  add_custom_target(guest_libs ALL DEPENDS "${SDK_LIB}/libblyt32.so"
                                           "${SDK_LIB_DEBUG}/libblyt32.so")
  return()
endif()

# ── libblytc.so — trimmed musl-based C library (ADR-0120) ───────────────────
file(GLOB LIBBLYTC_STRING_SRCS "${MUSL_DIR}/src/string/*.c")
# strsignal calls __lctrans_cur (locale translation); exclude it.
list(REMOVE_ITEM LIBBLYTC_STRING_SRCS "${MUSL_DIR}/src/string/strsignal.c")
file(GLOB LIBBLYTC_MATH_SRCS "${MUSL_DIR}/src/math/*.c")
file(GLOB LIBBLYTC_CTYPE_SRCS "${MUSL_DIR}/src/ctype/*.c")

set(LIBBLYTC_SRCS
    ${LIBBLYTC_STRING_SRCS}
    ${LIBBLYTC_MATH_SRCS}
    ${LIBBLYTC_CTYPE_SRCS}
    "${MUSL_DIR}/src/stdlib/strtol.c"
    "${MUSL_DIR}/src/stdlib/strtod.c"
    "${MUSL_DIR}/src/stdlib/qsort.c"
    "${MUSL_DIR}/src/stdlib/qsort_nr.c"
    "${MUSL_DIR}/src/stdlib/abs.c"
    "${MUSL_DIR}/src/stdlib/atoi.c"
    "${MUSL_DIR}/src/stdlib/atol.c"
    "${MUSL_DIR}/src/stdlib/atof.c"
    "${MUSL_DIR}/src/internal/floatscan.c"
    "${MUSL_DIR}/src/internal/intscan.c"
    "${MUSL_DIR}/src/internal/shgetc.c"
    "${MUSL_DIR}/src/stdio/vsnprintf.c"
    "${MUSL_DIR}/src/stdio/snprintf.c"
    "${MUSL_DIR}/src/stdio/vfprintf.c"
    "${MUSL_DIR}/src/stdio/fwrite.c"
    "${MUSL_DIR}/src/stdio/fmemopen.c"
    "${MUSL_DIR}/src/stdio/__overflow.c"
    "${MUSL_DIR}/src/stdio/__towrite.c"
    "${MUSL_DIR}/src/stdio/__toread.c"
    "${MUSL_DIR}/src/stdio/__uflow.c"
    "${MUSL_DIR}/src/stdio/__fmodeflags.c"
    "${CMAKE_SOURCE_DIR}/runtime/guest/src/libblytc/blytc_arena.c"
    "${CMAKE_SOURCE_DIR}/runtime/guest/src/libblytc/blytc_stubs.c")

set(LIBBLYTC_INCLUDES
    -I
    "${MUSL_DIR}/src/include" # defines `hidden` and other internal macros
    -I
    "${MUSL_DIR}/include"
    -I
    "${MUSL_DIR}/arch/riscv32"
    -I
    "${MUSL_DIR}/arch/generic"
    -I
    "${MUSL_DIR}/src/internal"
    -I
    "${LIBBLYTC_BITS_DIR}/..")

# Determinism (ADR-0007) and musl-compatibility compile flags.
set(LIBBLYTC_CFLAGS
    -ffp-contract=off
    -fno-fast-math
    -fno-strict-aliasing
    -Wno-unused-parameter
    -Wno-sign-compare
    -Wno-implicit-fallthrough
    -Wno-unused-variable
    -Wno-deprecated-non-prototype)

foreach(_var release debug)
  blyt_set_variant(${_var})
  blyt_guest_so(
    "${_VDIR}/libblytc.so"
    ${_VSTRIP}
    "Cross-compiling libblytc.so (${_var}, musl subset)"
    ARGS
    ${LIBBLYTC_INCLUDES}
    ${LIBBLYTC_CFLAGS}
    ${_VOPT}
    -Wl,-soname,libblytc.so
    -o
    "${_VDIR}/libblytc.so"
    ${LIBBLYTC_SRCS}
    DEPENDS
    ${LIBBLYTC_SRCS}
    "${LIBBLYTC_ALLTYPES_H}")
endforeach()
set(LIBBLYTC_OUT "${SDK_LIB}/libblytc.so")

# ── Berkeley SoftFloat — compiler-rt soft-double/quad ABI builtins ──────────
#
# clang emits calls to __adddf3/__muldf3/__udivdi3/__extendsfdf2/etc. for any
# cart code doing double or 64-bit-integer arithmetic on this hardware-single-
# float target.  musl does NOT provide these.  Compiled into BOTH libblyt32.so
# and libblyt32lua.so so every cart resolves them from the .so's .dynsym
# (cart_load.c's import allowlist already permits them, ADR-0112).  Same RISC-V
# NaN-propagation specialisation rv32emu uses on the host, so behaviour is
# bit-identical to the emulator.
set(SF_SRC "${CMAKE_SOURCE_DIR}/third_party/rv32emu/src/softfloat/source")
set(BLYT_HAVE_SOFTFLOAT FALSE)
if(EXISTS "${SF_SRC}/f64_add.c")
  set(BLYT_HAVE_SOFTFLOAT TRUE)
  set(SF_PLATFORM_DIR "${CMAKE_BINARY_DIR}/softfloat-rv32")
  file(MAKE_DIRECTORY "${SF_PLATFORM_DIR}")
  # Minimal platform.h: disable thread-local so softfloat_roundingMode is
  # global.
  file(WRITE "${SF_PLATFORM_DIR}/platform.h" "#define THREAD_LOCAL\n")

  # Core SoftFloat: all s_*.c and f32/f64/f128/conversion files.  Exclude extF80
  # (80-bit), M-variant (multi-word array), bf16, f16.
  file(GLOB SF_ALL "${SF_SRC}/*.c")
  foreach(
    _EXCL_PATTERN
    "${SF_SRC}/extF80*"
    "${SF_SRC}/*M_*"
    "${SF_SRC}/*M.*"
    "${SF_SRC}/bf16*"
    "${SF_SRC}/f16*"
    "${SF_SRC}/f32_to_extF80*"
    "${SF_SRC}/f64_to_extF80*"
    "${SF_SRC}/f128_to_extF80*")
    file(GLOB _EXCL_FILES "${_EXCL_PATTERN}")
    list(REMOVE_ITEM SF_ALL ${_EXCL_FILES})
  endforeach()
  # RISC-V NaN propagation specialisations (specialize.h declares them all).
  file(
    GLOB
    SF_RISCV
    "${SF_SRC}/RISCV/s_propagateNaNF16UI.c"
    "${SF_SRC}/RISCV/s_propagateNaNF32UI.c"
    "${SF_SRC}/RISCV/s_propagateNaNF64UI.c"
    "${SF_SRC}/RISCV/s_propagateNaNF128UI.c"
    "${SF_SRC}/RISCV/s_propagateNaNExtF80UI.c"
    "${SF_SRC}/RISCV/s_f16UIToCommonNaN.c"
    "${SF_SRC}/RISCV/s_f32UIToCommonNaN.c"
    "${SF_SRC}/RISCV/s_f64UIToCommonNaN.c"
    "${SF_SRC}/RISCV/s_f128UIToCommonNaN.c"
    "${SF_SRC}/RISCV/s_extF80UIToCommonNaN.c"
    "${SF_SRC}/RISCV/s_commonNaNToF16UI.c"
    "${SF_SRC}/RISCV/s_commonNaNToF32UI.c"
    "${SF_SRC}/RISCV/s_commonNaNToF64UI.c"
    "${SF_SRC}/RISCV/s_commonNaNToF128UI.c"
    "${SF_SRC}/RISCV/s_commonNaNToExtF80UI.c"
    "${SF_SRC}/RISCV/softfloat_raiseFlags.c")
  # Multi-word-array helpers needed by f128_mul even with SOFTFLOAT_FAST_INT64.
  set(SF_MWORD "${SF_SRC}/s_add256M.c" "${SF_SRC}/s_sub256M.c"
               "${SF_SRC}/s_mul128To256M.c" "${SF_SRC}/s_shiftRightJam256M.c")
  set(SF_INCLUDES -I "${SF_SRC}/include" -I "${SF_SRC}/RISCV" -I
                  "${SF_PLATFORM_DIR}")
  set(SF_DEFINES -DSOFTFLOAT_FAST_INT64=1 -DSOFTFLOAT_ROUND_ODD=1)
  set(SF_BUILTINS
      "${CMAKE_SOURCE_DIR}/runtime/guest/src/libblyt32lua/softfloat_builtins.c")
else()
  message(STATUS "Guest SoftFloat: rv32emu submodule not initialised — \
libblyt32.so will lack double/64-bit builtins")
  set(SF_ALL "")
  set(SF_RISCV "")
  set(SF_MWORD "")
  set(SF_INCLUDES "")
  set(SF_DEFINES "")
  set(SF_BUILTINS "")
endif()

# ── libblyt32.so — Blyt32 ECALL stubs (thick) ───────────────────────────────
#
# Self-contained for cart LINK TIME: absorbs libblytcommon sources and ALL
# libblytc sources so carts need only -lblyt32 at build time.  Exporting
# malloc/free/string/math directly from libblyt32.so's .dynsym lets lld resolve
# them without adding libblytc.so to the cart's DT_NEEDED.
#
# Runtime: libblyt32.so declares DT_NEEDED: libblytc.so (forced via
# --no-as-needed); the runtime's BFS dynamic loader picks it up transitively.
# First-wins symbol rule: libblyt32.so's baked-in copies win on the
# emulated/libretro path; libblytc.so's are shadowed but present for the
# hardware trusted-exec path where ld.so resolves against it directly.
foreach(_var release debug)
  blyt_set_variant(${_var})
  blyt_guest_so(
    "${_VDIR}/libblyt32.so"
    ${_VSTRIP}
    "Cross-compiling libblyt32.so (${_var})"
    ARGS
    ${LIBBLYTC_INCLUDES}
    ${LIBBLYTC_CFLAGS}
    ${SF_INCLUDES}
    ${SF_DEFINES}
    ${_VOPT}
    -Wl,-soname,libblyt32.so
    -Wl,--no-as-needed
    "${_VDIR}/libblytc.so"
    -Wl,--as-needed
    -o
    "${_VDIR}/libblyt32.so"
    "${CMAKE_SOURCE_DIR}/runtime/guest/src/libblyt32/blyt32.c"
    "${CMAKE_SOURCE_DIR}/runtime/guest/src/libblytcommon/blyt_common.c"
    ${LIBBLYTC_SRCS}
    ${SF_ALL}
    ${SF_RISCV}
    ${SF_MWORD}
    ${SF_BUILTINS}
    DEPENDS
    "${CMAKE_SOURCE_DIR}/runtime/guest/src/libblyt32/blyt32.c"
    "${CMAKE_SOURCE_DIR}/runtime/guest/src/libblytcommon/blyt_common.c"
    ${LIBBLYTC_SRCS}
    ${SF_ALL}
    ${SF_RISCV}
    ${SF_MWORD}
    ${SF_BUILTINS}
    "${BLYT_H}"
    "${BLYT32_H}"
    "${LIBBLYTC_ALLTYPES_H}"
    "${_VDIR}/libblytc.so")
endforeach()
set(LIBBLYT32_OUT "${SDK_LIB}/libblyt32.so")

set(_guest_lib_outputs
    "${SDK_LIB}/libblytcommon.so" "${SDK_LIB}/libblytc.so"
    "${SDK_LIB}/libblyt32.so" "${SDK_LIB_DEBUG}/libblytcommon.so"
    "${SDK_LIB_DEBUG}/libblytc.so" "${SDK_LIB_DEBUG}/libblyt32.so")

# ── Lua guest libraries (ADR-0025/0066/0111/0130) ───────────────────────────
set(LUA_DIR "${CMAKE_SOURCE_DIR}/third_party/lua")
if(NOT EXISTS "${LUA_DIR}/lvm.c")
  message(STATUS "Lua guest libraries: skipped \
(run: git submodule update --init third_party/lua)")
else()
  file(GLOB LUA_GUEST_SRCS "${LUA_DIR}/*.c")
  # Remove standalone interpreter, bytecode compiler, and excluded sandboxed
  # libs (no I/O, no OS access, no dlopen, no debug hooks; utf8 saves space).
  foreach(
    _EXCL
    "${LUA_DIR}/lua.c"
    "${LUA_DIR}/luac.c"
    "${LUA_DIR}/onelua.c"
    "${LUA_DIR}/liolib.c"
    "${LUA_DIR}/loslib.c"
    "${LUA_DIR}/loadlib.c"
    "${LUA_DIR}/ldblib.c"
    "${LUA_DIR}/lutf8lib.c")
    list(REMOVE_ITEM LUA_GUEST_SRCS "${_EXCL}")
  endforeach()

  # Public musl headers for Lua: the standard include paths minus musl's
  # internal src/include/, which defines `weak` as an attribute macro that
  # collides with Lua's `GCObject *weak` field in lstate.h.
  set(LUA_MUSL_INCLUDES
      -I
      "${MUSL_DIR}/include"
      -I
      "${MUSL_DIR}/arch/riscv32"
      -I
      "${MUSL_DIR}/arch/generic"
      -I
      "${LIBBLYTC_BITS_DIR}/..")

  # Fixed Lua hash seed (ADR-0130/ADR-0066): identical string hashing — and
  # therefore pairs()/lua_next order, table.sort pivots, math.random streams —
  # across the rv32 and WASM execution paths.  Must match every Lua build (guest
  # libraries, host WASM Lua, blyt-luac).  0x424C5954 = "BLYT".
  set(LUA_SEED_DEF "-Dluai_makeseed()=0x424C5954u")

  set(LUA32_SYM
      "${CMAKE_SOURCE_DIR}/runtime/guest/src/libblyt32lua/blyt32lua.sym")
  set(LUA32_DIR "${CMAKE_SOURCE_DIR}/runtime/guest/src/libblyt32lua")

  # libblytcommonlua.so — standalone sandboxed Lua VM (tooling use; carts get
  # the VM via libblyt32lua.so below).
  blyt_guest_so(
    "${SDK_LIB}/libblytcommonlua.so"
    FALSE
    "Cross-compiling libblytcommonlua.so"
    ARGS
    ${LUA_MUSL_INCLUDES}
    ${LIBBLYTC_CFLAGS}
    -DLUA_32BITS=1
    -DLUA_USE_LONGJMP=1
    ${LUA_SEED_DEF}
    -I
    "${LUA_DIR}"
    -Wl,-soname,libblytcommonlua.so
    -o
    "${SDK_LIB}/libblytcommonlua.so"
    ${LUA_GUEST_SRCS}
    DEPENDS
    ${LUA_GUEST_SRCS}
    "${LIBBLYTC_ALLTYPES_H}")
  list(APPEND _guest_lib_outputs "${SDK_LIB}/libblytcommonlua.so")

  # libblyt32lua.so — Lua VM embedded directly (exports lua_*/luaL_* in .dynsym)
  # + blyt32 API bindings + cart lifecycle + everything libblytc does NOT
  # provide (setjmp/longjmp, soft-float ops, stubs).  ADR-0129: the guest-side
  # DAP master hook (per-instruction stepping) is debug-only — the debug variant
  # defines BLYT_DAP=1 and links the master hook sources.
  foreach(_var release debug)
    blyt_set_variant(${_var})
    if("${_var}" STREQUAL "debug")
      set(_VLUA_DAP_FLAGS -DBLYT_DAP=1 -I
                          "${CMAKE_SOURCE_DIR}/runtime/host/src/dap")
      set(_VLUA_DAP_SRCS
          "${CMAKE_SOURCE_DIR}/runtime/host/src/dap/master_hook.c"
          "${LUA32_DIR}/master_hook_ecall.c")
    else()
      set(_VLUA_DAP_FLAGS "")
      set(_VLUA_DAP_SRCS "")
    endif()
    blyt_guest_so(
      "${_VDIR}/libblyt32lua.so"
      ${_VSTRIP}
      "Cross-compiling libblyt32lua.so (${_var})"
      ARGS
      ${LUA_MUSL_INCLUDES}
      ${LIBBLYTC_CFLAGS}
      ${_VOPT}
      -DLUA_32BITS=1
      -DLUA_USE_LONGJMP=1
      ${LUA_SEED_DEF}
      ${_VLUA_DAP_FLAGS}
      -I
      "${LUA_DIR}"
      ${SF_INCLUDES}
      ${SF_DEFINES}
      -Wl,-soname,libblyt32lua.so
      "-Wl,--version-script,${LUA32_SYM}"
      -Wl,--as-needed
      "${_VDIR}/libblyt32.so"
      -o
      "${_VDIR}/libblyt32lua.so"
      ${LUA_GUEST_SRCS}
      "${MUSL_DIR}/src/setjmp/riscv32/setjmp.S"
      "${MUSL_DIR}/src/setjmp/riscv32/longjmp.S"
      ${SF_ALL}
      ${SF_RISCV}
      ${SF_MWORD}
      "${LUA32_DIR}/softfloat_builtins.c"
      "${LUA32_DIR}/lua_runtime_stubs.c"
      "${LUA32_DIR}/blyt32lua.c"
      ${_VLUA_DAP_SRCS}
      DEPENDS
      ${LUA_GUEST_SRCS}
      ${SF_ALL}
      ${SF_RISCV}
      ${SF_MWORD}
      "${LUA32_DIR}/softfloat_builtins.c"
      "${LUA32_DIR}/lua_runtime_stubs.c"
      "${LUA32_DIR}/blyt32lua.c"
      ${_VLUA_DAP_SRCS}
      "${LUA32_SYM}"
      "${BLYT_LUA_INTERNAL_H}"
      "${_VDIR}/libblyt32.so")
    list(APPEND _guest_lib_outputs "${_VDIR}/libblyt32lua.so")
  endforeach()
  set(LIBBLYT32LUA_OUT "${SDK_LIB}/libblyt32lua.so")

  # libblyt32lua-bridge.so (ADR-0130) — the WASM-target variant: no Lua VM,
  # every lua_* export is a BLYT_ECALL_LUA_OP stub serviced by the host.  Same
  # soname and export surface (blyt32lua.sym), so the WASM frontend embeds it
  # under the name "libblyt32lua.so" and carts need no rebuild.
  set(LUA_BRIDGE_SRC
      "${CMAKE_SOURCE_DIR}/runtime/guest/src/libblyt32lua-bridge/blyt32lua_bridge.c"
  )
  foreach(_var release debug)
    blyt_set_variant(${_var})
    blyt_guest_so(
      "${_VDIR}/libblyt32lua-bridge.so"
      ${_VSTRIP}
      "Cross-compiling libblyt32lua-bridge.so (${_var})"
      ARGS
      ${LIBBLYTC_CFLAGS}
      ${_VOPT}
      -Wl,-soname,libblyt32lua.so
      "-Wl,--version-script,${LUA32_SYM}"
      -Wl,--no-as-needed
      "${_VDIR}/libblyt32.so"
      -Wl,--as-needed
      -o
      "${_VDIR}/libblyt32lua-bridge.so"
      "${LUA_BRIDGE_SRC}"
      DEPENDS
      "${LUA_BRIDGE_SRC}"
      "${LUA32_SYM}"
      "${BLYT_LUA_INTERNAL_H}"
      "${_VDIR}/libblyt32.so")
    list(APPEND _guest_lib_outputs "${_VDIR}/libblyt32lua-bridge.so")
  endforeach()

  # blyt-luac — host-native Lua bytecode compiler (LUA_32BITS=1 to match the
  # guest VMs' 4-byte lua_Integer / lua_Number).
  file(GLOB LUA_HOST_SRCS "${LUA_DIR}/*.c")
  foreach(_EXCL "${LUA_DIR}/lua.c" "${LUA_DIR}/onelua.c" "${LUA_DIR}/ltests.c")
    list(REMOVE_ITEM LUA_HOST_SRCS "${_EXCL}")
  endforeach()
  add_custom_command(
    OUTPUT "${SDK_BIN}/blyt-luac"
    COMMAND
      "${BLYT_RV32_CLANG}" -DLUA_32BITS=1 ${LUA_SEED_DEF} -O2 -I "${LUA_DIR}"
      -Wno-unused-parameter -Wno-sign-compare -Wno-implicit-fallthrough
      -Wno-deprecated-non-prototype -o "${SDK_BIN}/blyt-luac" ${LUA_HOST_SRCS}
      "${CMAKE_SOURCE_DIR}/runtime/tools/blyt-luac.c" -lm
    DEPENDS ${LUA_HOST_SRCS} "${CMAKE_SOURCE_DIR}/runtime/tools/blyt-luac.c"
    COMMENT "Compiling blyt-luac (host-native, LUA_32BITS=1)"
    VERBATIM)
  list(APPEND _guest_lib_outputs "${SDK_BIN}/blyt-luac")
endif()

add_custom_target(guest_libs ALL DEPENDS ${_guest_lib_outputs})
message(STATUS "Guest libraries → ${SDK_LIB} (release + debug)")

# ═════════════════════════════════════════════════════════════════════════════
# Native trusted-exec guest libs (sdk/lib/native) — RISC-V QEMU gate
#
# /lib/ld-blyt.so.1 on hardware is the system musl ILP32 interpreter.  The
# native chain: libblyt32.so → libblytc.so (thin DT_NEEDED carrier) →
# ld-blyt.so.1, which makes malloc/getenv/snprintf/… resolvable at runtime.
# ═════════════════════════════════════════════════════════════════════════════
if(BLYT_BUILD_NATIVE)
  file(MAKE_DIRECTORY "${SDK_LIB_NATIVE}" "${CMAKE_BINARY_DIR}/native")

  # ld-blyt.so.1 stub: empty shared lib with SONAME=ld-blyt.so.1.  Used only at
  # link time so lld emits DT_NEEDED: ld-blyt.so.1 in libblytc.so.  Never staged
  # to QEMU; the real /lib/ld-blyt.so.1 (system musl) is loaded by the kernel as
  # the ELF interpreter.
  set(LD_BLYT_STUB_SRC
      "${CMAKE_SOURCE_DIR}/frontends/native/src/libblytc/ld_blyt_stub.c")
  set(LD_BLYT_STUB_OUT "${CMAKE_BINARY_DIR}/native/ld-blyt.so.1.stub")
  blyt_guest_so(
    "${LD_BLYT_STUB_OUT}"
    FALSE
    "Cross-compiling ld-blyt.so.1 stub"
    ARGS
    -O2
    -Wl,-soname,ld-blyt.so.1
    -o
    "${LD_BLYT_STUB_OUT}"
    "${LD_BLYT_STUB_SRC}"
    DEPENDS
    "${LD_BLYT_STUB_SRC}")

  # Native libblytc.so: thin wrapper whose only job is to carry DT_NEEDED:
  # ld-blyt.so.1 so carts and libblyt32.so can resolve stdlib symbols from the
  # system musl interpreter at runtime on QEMU.
  set(LIBBLYTC_NATIVE_SRC
      "${CMAKE_SOURCE_DIR}/frontends/native/src/libblytc/libblytc_native.c")
  blyt_guest_so(
    "${SDK_LIB_NATIVE}/libblytc.so"
    FALSE
    "Cross-compiling libblytc.so (native, DT_NEEDED: ld-blyt.so.1)"
    ARGS
    -O2
    -Wl,-soname,libblytc.so
    -Wl,-Bdynamic
    -Wl,--no-as-needed
    "${LD_BLYT_STUB_OUT}"
    -Wl,--as-needed
    -o
    "${SDK_LIB_NATIVE}/libblytc.so"
    "${LIBBLYTC_NATIVE_SRC}"
    DEPENDS
    "${LIBBLYTC_NATIVE_SRC}"
    "${LD_BLYT_STUB_OUT}")

  # Native libblyt32.so: real API implementations (blyt_console_debug →
  # write(2), restricted seccomp constructor).  Keep -O0: blyt32.c uses
  # hand-rolled string helpers prefixed blyt32_native_* so the compiler won't
  # recognise them as stdlib functions; -O0 avoids loop-idiom rewrites into
  # libcalls.
  set(LIBBLYT32_NATIVE_SRC
      "${CMAKE_SOURCE_DIR}/frontends/native/src/libblyt32/blyt32.c")
  set(LIBBLYT32_NATIVE_INC "${CMAKE_SOURCE_DIR}/frontends/native/src/libblyt32")
  blyt_guest_so(
    "${SDK_LIB_NATIVE}/libblyt32.so"
    FALSE
    "Cross-compiling libblyt32.so (native)"
    ARGS
    -O0
    -Wl,-soname,libblyt32.so
    -I
    "${LIBBLYT32_NATIVE_INC}"
    -Wl,-Bdynamic
    -Wl,--no-as-needed
    "${SDK_LIB}/libblytcommon.so"
    "${SDK_LIB_NATIVE}/libblytc.so"
    -Wl,--as-needed
    -o
    "${SDK_LIB_NATIVE}/libblyt32.so"
    "${LIBBLYT32_NATIVE_SRC}"
    DEPENDS
    "${LIBBLYT32_NATIVE_SRC}"
    "${LIBBLYT32_NATIVE_INC}/seccomp_restricted.h"
    "${BLYT_H}"
    "${SDK_LIB}/libblytcommon.so"
    "${SDK_LIB_NATIVE}/libblytc.so")

  # Stage libblytcommon.so alongside so one LD_LIBRARY_PATH covers the set.
  add_custom_command(
    OUTPUT "${SDK_LIB_NATIVE}/libblytcommon.so"
    COMMAND "${CMAKE_COMMAND}" -E copy "${SDK_LIB}/libblytcommon.so"
            "${SDK_LIB_NATIVE}/libblytcommon.so"
    DEPENDS "${SDK_LIB}/libblytcommon.so"
    COMMENT "Staging libblytcommon.so to sdk/lib/native/"
    VERBATIM)

  set(_native_outputs
      "${SDK_LIB_NATIVE}/libblyt32.so" "${SDK_LIB_NATIVE}/libblytc.so"
      "${SDK_LIB_NATIVE}/libblytcommon.so")

  # Native libblyt32lua.so — Lua VM + bindings for trusted native exec.
  # ld-blyt.so.1 does not export libc symbols under standard names, so the
  # needed functionality is embedded directly (curated musl subset compiled as a
  # separate partial-link object to dodge the musl `weak` macro vs Lua lstate.h
  # collision; mmap-based malloc; native stdio/luaopen stubs).
  if(EXISTS "${LUA_DIR}/lvm.c" AND BLYT_HAVE_SOFTFLOAT)
    # Curated musl subset → single partial-link object, compiled with
    # LIBBLYTC_INCLUDES (musl internal headers).  Excluded vs LIBBLYTC_SRCS:
    # fwrite/fmemopen (FILE* internals; stubbed natively), blytc_arena
    # (emulated-path arena), blytc_stubs (ECALL-based).
    set(_LIBBLYTC_NATIVE_SRCS
        ${LIBBLYTC_STRING_SRCS}
        ${LIBBLYTC_MATH_SRCS}
        ${LIBBLYTC_CTYPE_SRCS}
        "${MUSL_DIR}/src/stdlib/strtol.c"
        "${MUSL_DIR}/src/stdlib/strtod.c"
        "${MUSL_DIR}/src/stdlib/qsort.c"
        "${MUSL_DIR}/src/stdlib/qsort_nr.c"
        "${MUSL_DIR}/src/stdlib/abs.c"
        "${MUSL_DIR}/src/stdlib/atoi.c"
        "${MUSL_DIR}/src/stdlib/atol.c"
        "${MUSL_DIR}/src/stdlib/atof.c"
        "${MUSL_DIR}/src/internal/floatscan.c"
        "${MUSL_DIR}/src/internal/intscan.c"
        "${MUSL_DIR}/src/internal/shgetc.c"
        "${MUSL_DIR}/src/stdio/vsnprintf.c"
        "${MUSL_DIR}/src/stdio/snprintf.c"
        "${MUSL_DIR}/src/stdio/vfprintf.c"
        "${LUA32_DIR}/lua_native_fwritex.c"
        "${MUSL_DIR}/src/stdio/__overflow.c"
        "${MUSL_DIR}/src/stdio/__towrite.c"
        "${MUSL_DIR}/src/stdio/__toread.c"
        "${MUSL_DIR}/src/stdio/__uflow.c"
        "${MUSL_DIR}/src/stdio/__fmodeflags.c")
    set(_LIBBLYTC_NATIVE_OBJ "${CMAKE_BINARY_DIR}/libblytc_native.o")
    # Partial-link flags: same target/arch/ABI as RV32_BASE but no -shared.
    # -no-pie prevents the clang driver injecting -pie into the lld command,
    # which would conflict with -r (relocatable partial link).
    set(_RV32_PARTIAL
        --target=riscv32-linux-gnu
        -march=rv32imafc
        -mabi=ilp32f
        -fPIC
        -nostdlib
        -no-pie
        "-fuse-ld=${BLYT_RV32_LLD}")
    add_custom_command(
      OUTPUT "${_LIBBLYTC_NATIVE_OBJ}"
      COMMAND
        "${BLYT_RV32_CLANG}" ${_RV32_PARTIAL} ${LIBBLYTC_INCLUDES}
        ${LIBBLYTC_CFLAGS} -O2 "-Wl,-r" -o "${_LIBBLYTC_NATIVE_OBJ}"
        ${_LIBBLYTC_NATIVE_SRCS}
      DEPENDS ${_LIBBLYTC_NATIVE_SRCS} "${LIBBLYTC_ALLTYPES_H}"
      COMMENT "Partial-linking libblytc_native.o"
      VERBATIM)

    # libc-stub.so: zero-symbol stub with SONAME=libc.so.  musl's is_self path
    # adds the ld.so to the symbol chain for any reserved-name DT_NEEDED (libc.,
    # libm., …); symbols ld.so exports under standard names become available.
    set(_LIBC_STUB "${CMAKE_BINARY_DIR}/libc-stub.so")
    blyt_guest_so(
      "${_LIBC_STUB}"
      FALSE
      "Cross-compiling libc-stub.so"
      ARGS
      -Wl,-soname,libc.so
      -x
      c
      /dev/null
      -o
      "${_LIBC_STUB}")

    blyt_guest_so(
      "${SDK_LIB_NATIVE}/libblyt32lua.so"
      TRUE
      "Cross-compiling libblyt32lua.so (native)"
      ARGS
      ${LUA_MUSL_INCLUDES}
      ${LIBBLYTC_CFLAGS}
      -O2
      -DLUA_32BITS=1
      -DLUA_USE_LONGJMP=1
      ${LUA_SEED_DEF}
      -I
      "${LUA_DIR}"
      ${SF_INCLUDES}
      ${SF_DEFINES}
      -Wl,-soname,libblyt32lua.so
      "-Wl,--version-script,${LUA32_SYM}"
      -Wl,-z,now
      # --no-as-needed forces DT_NEEDED: libc.so even though the stub exports
      # nothing; the is_self path adds ldso to the symbol chain.
      -Wl,--no-as-needed
      "${_LIBC_STUB}"
      -Wl,--as-needed
      "${SDK_LIB_NATIVE}/libblyt32.so"
      -o
      "${SDK_LIB_NATIVE}/libblyt32lua.so"
      ${LUA_GUEST_SRCS}
      "${MUSL_DIR}/src/setjmp/riscv32/setjmp.S"
      "${MUSL_DIR}/src/setjmp/riscv32/longjmp.S"
      ${SF_ALL}
      ${SF_RISCV}
      ${SF_MWORD}
      "${LUA32_DIR}/softfloat_builtins.c"
      "${_LIBBLYTC_NATIVE_OBJ}"
      "${LUA32_DIR}/lua_native_malloc.c"
      "${LUA32_DIR}/lua_native_stubs.c"
      "${LUA32_DIR}/blyt32lua.c"
      DEPENDS
      ${LUA_GUEST_SRCS}
      ${SF_ALL}
      ${SF_RISCV}
      ${SF_MWORD}
      "${LUA32_DIR}/softfloat_builtins.c"
      "${_LIBBLYTC_NATIVE_OBJ}"
      "${LUA32_DIR}/lua_native_malloc.c"
      "${LUA32_DIR}/lua_native_stubs.c"
      "${LUA32_DIR}/blyt32lua.c"
      "${LUA32_SYM}"
      "${_LIBC_STUB}"
      "${SDK_LIB_NATIVE}/libblyt32.so")
    list(APPEND _native_outputs "${SDK_LIB_NATIVE}/libblyt32lua.so")
  endif()

  add_custom_target(libblyt32_native_so ALL DEPENDS ${_native_outputs})
  message(STATUS "Native guest libraries → ${SDK_LIB_NATIVE}")
else()
  message(STATUS "Native guest libraries: skipped (BLYT_BUILD_NATIVE=OFF)")
endif()
