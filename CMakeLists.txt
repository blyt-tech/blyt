cmake_minimum_required(VERSION 3.25)
project(blyt C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Version (ADR-0128): copy version.txt to build/version.txt if not already
# present (CI pre-creates it with a timestamp; local dev gets a verbatim copy).
if(NOT EXISTS "${CMAKE_BINARY_DIR}/version.txt")
  file(COPY "${CMAKE_SOURCE_DIR}/version.txt" DESTINATION "${CMAKE_BINARY_DIR}")
endif()
file(READ "${CMAKE_BINARY_DIR}/version.txt" BLYT_VERSION)
string(STRIP "${BLYT_VERSION}" BLYT_VERSION)
message(STATUS "blyt version: ${BLYT_VERSION}")

# flatcc — pure C FlatBuffers schema compiler, built from source (ADR-0126)
set(FLATCC_TEST
    OFF
    CACHE BOOL "" FORCE)
set(FLATCC_CXX_TEST
    OFF
    CACHE BOOL "" FORCE)
set(FLATCC_INSTALL
    OFF
    CACHE BOOL "" FORCE)
set(FLATCC_DEBUG_CLANG_SANITIZE
    OFF
    CACHE BOOL "" FORCE)
add_subdirectory(third_party/flatcc EXCLUDE_FROM_ALL)
# flatcc hardcodes PROJECT_SOURCE_DIR/lib as the archive output, which fails
# when the source is mounted read-only (e.g. Docker :ro). Override it here so
# all targets write to the build tree instead.
foreach(_flatcc_tgt flatcc flatccrt flatcc_cli)
  if(TARGET ${_flatcc_tgt})
    set_target_properties(
      ${_flatcc_tgt}
      PROPERTIES ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/flatcc-lib"
                 LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/flatcc-lib"
                 RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/flatcc-bin")
  endif()
endforeach()

# FlatBuffers codegen — generates pure C reader headers for the runtime
# (ADR-0126)
#
# Cross-compilation: set BLYT_PREGENERATED_DIR to the flatbuffers/ directory
# from a prior host build.  The code-generation step (which would try to execute
# a target-arch flatcc_cli on the host) is skipped; the host-generated headers
# are reused instead (they are architecture-independent).
set(SCHEMAS_DIR "${CMAKE_SOURCE_DIR}/schemas")

if(DEFINED BLYT_PREGENERATED_DIR)
  set(GENERATED_DIR "${BLYT_PREGENERATED_DIR}")
  add_custom_target(flatbuffers_generated)
  message(
    STATUS "FlatBuffers: reusing pre-generated headers from ${GENERATED_DIR}")
else()
  set(GENERATED_DIR "${CMAKE_BINARY_DIR}/generated/flatbuffers")
  file(MAKE_DIRECTORY "${GENERATED_DIR}")
  add_custom_command(
    OUTPUT "${GENERATED_DIR}/cart_info_reader.h"
           "${GENERATED_DIR}/cart_info_verifier.h"
           "${GENERATED_DIR}/cart_config_reader.h"
           "${GENERATED_DIR}/cart_config_verifier.h"
           "${GENERATED_DIR}/flatbuffers_common_reader.h"
    COMMAND
      "$<TARGET_FILE:flatcc_cli>" --common_reader --reader --verifier -o
      "${GENERATED_DIR}" "${SCHEMAS_DIR}/cart_info.fbs"
      "${SCHEMAS_DIR}/cart_config.fbs"
    DEPENDS flatcc_cli "${SCHEMAS_DIR}/cart_info.fbs"
            "${SCHEMAS_DIR}/cart_config.fbs"
    COMMENT "Generating FlatBuffers C headers"
    VERBATIM)
  add_custom_target(
    flatbuffers_generated ALL
    DEPENDS "${GENERATED_DIR}/cart_info_reader.h"
            "${GENERATED_DIR}/cart_info_verifier.h"
            "${GENERATED_DIR}/cart_config_reader.h"
            "${GENERATED_DIR}/cart_config_verifier.h"
            "${GENERATED_DIR}/flatbuffers_common_reader.h")
endif()

# -------------------------------------------------------------------------
# Berkeley SoftFloat — IEEE 754 software FP for the F extension. rv32emu's
# Makefile generates an empty platform.h; we replicate that here.
# -------------------------------------------------------------------------
set(SOFTFLOAT_SRC
    "${CMAKE_SOURCE_DIR}/third_party/rv32emu/src/softfloat/source")
set(SOFTFLOAT_PLATFORM_DIR "${CMAKE_BINARY_DIR}/softfloat")
file(MAKE_DIRECTORY "${SOFTFLOAT_PLATFORM_DIR}")
file(WRITE "${SOFTFLOAT_PLATFORM_DIR}/platform.h" "")

# rv32emu's Makefile compiles a specific subset of softfloat sources for the
# SOFTFLOAT_FAST_INT64 path.  The non-fast-int64 (*M.c helpers, BF16, and a
# handful of other files) must be excluded.  We glob the source directory then
# remove those files explicitly.
file(GLOB SOFTFLOAT_SOURCES "${SOFTFLOAT_SRC}/*.c")
set(SOFTFLOAT_EXCLUDE_NAMES
    # Non-FAST_INT64 M-word-array helpers
    bf16_isSignalingNaN.c
    bf16_to_f32.c
    f32_to_bf16.c
    s_addCarryM.c
    s_addComplCarryM.c
    s_addExtF80M.c
    s_addF128M.c
    s_addM.c
    s_compare128M.c
    s_compare96M.c
    s_compareNonnormExtF80M.c
    s_invalidExtF80M.c
    s_invalidF128M.c
    s_isNaNF128M.c
    s_mul128MTo256M.c
    s_mul64To128M.c
    s_mulAddF128M.c
    s_negXM.c
    s_normExtF80SigM.c
    s_normRoundPackMToExtF80M.c
    s_normRoundPackMToF128M.c
    s_normSubnormalBF16Sig.c
    s_normSubnormalF128SigM.c
    s_remStepMBy32.c
    s_roundMToI64.c
    s_roundMToUI64.c
    s_roundPackMToExtF80M.c
    s_roundPackMToF128M.c
    s_roundPackToBF16.c
    s_shiftLeftM.c
    s_shiftNormSigF128M.c
    s_shiftRightJamM.c
    s_shiftRightM.c
    s_shortShiftLeft64To96M.c
    s_shortShiftLeftM.c
    s_shortShiftRightExtendM.c
    s_shortShiftRightJamM.c
    s_shortShiftRightM.c
    s_sub1XM.c
    s_subM.c
    s_tryPropagateNaNExtF80M.c
    s_tryPropagateNaNF128M.c)
foreach(f ${SOFTFLOAT_EXCLUDE_NAMES})
  list(REMOVE_ITEM SOFTFLOAT_SOURCES "${SOFTFLOAT_SRC}/${f}")
endforeach()
# Add the five RISCV-specialization files (from SOFTFLOAT_OBJS_SPECIALIZE)
list(
  APPEND
  SOFTFLOAT_SOURCES
  "${SOFTFLOAT_SRC}/RISCV/softfloat_raiseFlags.c"
  "${SOFTFLOAT_SRC}/RISCV/s_propagateNaNF16UI.c"
  "${SOFTFLOAT_SRC}/RISCV/s_propagateNaNF32UI.c"
  "${SOFTFLOAT_SRC}/RISCV/s_propagateNaNF64UI.c"
  "${SOFTFLOAT_SRC}/RISCV/s_propagateNaNF128UI.c")

add_library(libsoftfloat STATIC ${SOFTFLOAT_SOURCES})
target_include_directories(
  libsoftfloat PUBLIC "${SOFTFLOAT_SRC}/include" "${SOFTFLOAT_SRC}/RISCV"
                      "${SOFTFLOAT_PLATFORM_DIR}")
target_compile_options(
  libsoftfloat
  PRIVATE -DLITTLEENDIAN=1
          -DINLINE_LEVEL=5
          -DSOFTFLOAT_ROUND_ODD
          -DSOFTFLOAT_FAST_INT64
          -DSOFTFLOAT_FAST_DIV64TO32
          "-DINLINE=static inline"
          -Wno-unused-parameter
          -Wno-unused-variable
          -Wno-sign-compare
          -Wno-implicit-fallthrough
          -Wno-uninitialized)

# -------------------------------------------------------------------------
# rv32emu — user-mode RISC-V emulator, built from source as a static library. We
# compile only the user-mode sources (no JIT, no SDL, no system/MMU, no
# GDBSTUB).
# -------------------------------------------------------------------------
set(RV32EMU_DIR "${CMAKE_SOURCE_DIR}/third_party/rv32emu/src")

set(RV32EMU_SOURCES
    ${RV32EMU_DIR}/riscv.c
    ${RV32EMU_DIR}/emulate.c
    ${RV32EMU_DIR}/decode.c
    ${RV32EMU_DIR}/elf.c
    ${RV32EMU_DIR}/io.c
    ${RV32EMU_DIR}/map.c
    ${RV32EMU_DIR}/mpool.c
    ${RV32EMU_DIR}/cache.c
    ${RV32EMU_DIR}/syscall.c
    ${RV32EMU_DIR}/utils.c
    ${RV32EMU_DIR}/log.c)

add_library(libblytemu STATIC ${RV32EMU_SOURCES})

# Feature definitions used by all targets that include rv32emu headers.
set(RV32EMU_FEATURE_DEFS
    RV32_FEATURE_SDL=0
    RV32_FEATURE_GDBSTUB=0
    RV32_FEATURE_JIT=0
    RV32_FEATURE_T2C=0
    RV32_FEATURE_SYSTEM=0
    RV32_FEATURE_ELF_LOADER=0
    RV32_FEATURE_ARCH_TEST=0)

# For libblytemu: force-include common.h so that every rv32emu .c file gets
# RV32_HAS, FORCE_INLINE, HAVE_MMAP, etc. without needing to include it
# explicitly (rv32emu's own Makefile uses a generated config.h for this).
#
# Generate a single wrapper header that includes common.h then, on macOS only,
# overrides HAVE_MMAP back to 0.  Using one force-include avoids cmake
# deduplicating two -include flags with the same name.
#
# rv32emu uses PROT_NONE demand-paging (HAVE_MMAP=1) on all platforms, which
# gives the OS freedom to place the 256 MiB guest region at a safe address. On
# macOS Tahoe+, malloc(256 MiB) returns addresses in the 0x200000000 range that
# overlap __OBJC_RO, causing SIGBUS; mmap does not have this problem. LLDB
# intercepts the initial demand-paging faults as EXC_BAD_ACCESS — handled via a
# signal mask in launch.json ("process handle SIGSEGV -n false -p false").
set(RV32EMU_CONFIG_H "${CMAKE_BINARY_DIR}/rv32emu_config.h")
file(WRITE "${RV32EMU_CONFIG_H}" "#include \"${RV32EMU_DIR}/common.h\"\n")

target_compile_definitions(libblytemu PRIVATE ${RV32EMU_FEATURE_DEFS})
target_compile_options(libblytemu PRIVATE -include "${RV32EMU_CONFIG_H}")
target_include_directories(libblytemu PUBLIC ${RV32EMU_DIR})
target_link_libraries(libblytemu PRIVATE libsoftfloat)

# -------------------------------------------------------------------------
# Guest-side RV32IMAFC shared libraries
#
# libblytcommon.so — variant-portable shared API (blyt.h); ECALL stubs for all
# functions declared in blyt.h. libblytc.so — runtime-provided C library
# (ADR-0120): trimmed musl subset (malloc, string, math, printf, fmemopen).
# Requires third_party/musl. libblyt32.so — Blyt32 variant library; DT_NEEDED:
# libblytcommon.so, libblytc.so.  Carts link against this and get the C library
# transitively.
#
# Requires clang with riscv32 target support and lld.
# -------------------------------------------------------------------------
find_program(CLANG_RV32 clang)
find_program(LLD_FOUND ld.lld)
if(CLANG_RV32 AND LLD_FOUND)
  # Verify that this clang can actually target riscv32 (Apple clang cannot).
  execute_process(
    COMMAND ${CLANG_RV32} --target=riscv32 -march=rv32imafc -mabi=ilp32f -x c -E
            - -o /dev/null
    INPUT_FILE /dev/null
    RESULT_VARIABLE CLANG_RV32_OK
    ERROR_QUIET OUTPUT_QUIET)
endif()
if(CLANG_RV32
   AND LLD_FOUND
   AND CLANG_RV32_OK EQUAL 0)
  set(RV32_FLAGS
      --target=riscv32
      -march=rv32imafc
      -mabi=ilp32f
      -shared
      # -Wl,--shared explicitly tells lld to produce ET_DYN; clang may inject
      # -Bstatic for bare-metal riscv targets which overrides -shared otherwise.
      -Wl,--shared
      -fPIC
      -nostdlib
      -fuse-ld=lld)
  set(RV32_INCLUDE -I "${CMAKE_SOURCE_DIR}/runtime/guest/include")
  set(BLYT_H "${CMAKE_SOURCE_DIR}/runtime/guest/include/blyt.h")

  # libblytcommon.so — variant-portable blyt.h ECALL stubs
  set(LIBBLYTCOMMON_SRC
      "${CMAKE_SOURCE_DIR}/runtime/guest/src/libblytcommon/blyt_common.c")
  set(LIBBLYTCOMMON_OUT "${CMAKE_BINARY_DIR}/libblytcommon.so")
  add_custom_command(
    OUTPUT "${LIBBLYTCOMMON_OUT}"
    COMMAND "${CLANG_RV32}" ${RV32_FLAGS} -Wl,-soname,libblytcommon.so
            ${RV32_INCLUDE} -o "${LIBBLYTCOMMON_OUT}" "${LIBBLYTCOMMON_SRC}"
    DEPENDS "${LIBBLYTCOMMON_SRC}" "${BLYT_H}"
    COMMENT "Cross-compiling libblytcommon.so (RV32IMAFC)"
    VERBATIM)

  # ---- libblytc.so — runtime-provided C library (ADR-0120) ----
  #
  # Built from a curated subset of musl 1.2 sources (third_party/musl) plus our
  # own arena allocator and internal stubs.  Requires the musl submodule.
  set(MUSL_DIR "${CMAKE_SOURCE_DIR}/third_party/musl")
  set(LIBBLYTC_BITS_DIR "${CMAKE_BINARY_DIR}/libblytc/bits")
  set(LIBBLYTC_ALLTYPES_H "${LIBBLYTC_BITS_DIR}/alltypes.h")
  set(LIBBLYTC_OUT "${CMAKE_BINARY_DIR}/libblytc.so")

  if(EXISTS "${MUSL_DIR}/include/stdio.h")
    # Generate bits/alltypes.h from the musl arch + generic templates. Set up
    # bits/ directory: copy all arch/riscv32/bits/*.h, then generate the two
    # that need processing.  Runs at configure time.
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
      RESULT_VARIABLE ALLTYPES_RESULT
      ERROR_QUIET)

    if(NOT ALLTYPES_RESULT EQUAL 0)
      message(
        WARNING "libblytc.so: failed to generate bits/alltypes.h — skipping")
      set(MUSL_DIR "")
    endif()

    # Generate bits/syscall.h: copy .in then append SYS_xxx alias for each
    # __NR_xxx (matching musl's Makefile rule).
    if(MUSL_DIR)
      execute_process(
        COMMAND
          sh -c "cp '${MUSL_DIR}/arch/riscv32/bits/syscall.h.in' \
              '${LIBBLYTC_BITS_DIR}/syscall.h' \
           && sed -n -e 's/__NR_/SYS_/p' \
               '${MUSL_DIR}/arch/riscv32/bits/syscall.h.in' \
               >> '${LIBBLYTC_BITS_DIR}/syscall.h'"
        RESULT_VARIABLE SYSCALL_RESULT
        ERROR_QUIET)
      if(NOT SYSCALL_RESULT EQUAL 0)
        message(WARNING "libblytc.so: failed to generate bits/syscall.h")
        set(MUSL_DIR "")
      endif()
    endif()
  else()
    message(STATUS "libblytc.so: third_party/musl not initialised — skipping \
(run: git submodule update --init third_party/musl)")
    set(MUSL_DIR "")
  endif()

  if(MUSL_DIR)
    # musl source subsets — generic C only (no arch-specific subdirectories).
    file(GLOB LIBBLYTC_STRING_SRCS "${MUSL_DIR}/src/string/*.c")
    # strsignal calls __lctrans_cur (locale translation); exclude it.
    list(REMOVE_ITEM LIBBLYTC_STRING_SRCS "${MUSL_DIR}/src/string/strsignal.c")
    file(GLOB LIBBLYTC_MATH_SRCS "${MUSL_DIR}/src/math/*.c")
    file(GLOB LIBBLYTC_CTYPE_SRCS "${MUSL_DIR}/src/ctype/*.c")

    set(LIBBLYTC_STDLIB_SRCS
        "${MUSL_DIR}/src/stdlib/strtol.c"
        "${MUSL_DIR}/src/stdlib/strtod.c"
        "${MUSL_DIR}/src/stdlib/qsort.c"
        "${MUSL_DIR}/src/stdlib/qsort_nr.c"
        "${MUSL_DIR}/src/stdlib/abs.c"
        "${MUSL_DIR}/src/stdlib/atoi.c"
        "${MUSL_DIR}/src/stdlib/atol.c"
        "${MUSL_DIR}/src/stdlib/atof.c")

    # floatscan / intscan / shgetc are internal helpers for strtod / strtol.
    set(LIBBLYTC_INTERNAL_SRCS
        "${MUSL_DIR}/src/internal/floatscan.c"
        "${MUSL_DIR}/src/internal/intscan.c"
        "${MUSL_DIR}/src/internal/shgetc.c")

    # stdio subset: vsnprintf / snprintf / vfprintf / fmemopen + minimal FILE
    # infrastructure.  Excludes anything that issues real syscalls (fopen,
    # fread, fwrite via fd, etc.).
    set(LIBBLYTC_STDIO_SRCS
        "${MUSL_DIR}/src/stdio/vsnprintf.c"
        "${MUSL_DIR}/src/stdio/snprintf.c"
        "${MUSL_DIR}/src/stdio/vfprintf.c"
        "${MUSL_DIR}/src/stdio/fwrite.c"
        "${MUSL_DIR}/src/stdio/fmemopen.c"
        "${MUSL_DIR}/src/stdio/__overflow.c"
        "${MUSL_DIR}/src/stdio/__towrite.c"
        "${MUSL_DIR}/src/stdio/__toread.c"
        "${MUSL_DIR}/src/stdio/__uflow.c"
        "${MUSL_DIR}/src/stdio/__fmodeflags.c")

    set(LIBBLYTC_OWN_SRCS
        "${CMAKE_SOURCE_DIR}/runtime/guest/src/libblytc/blytc_arena.c"
        "${CMAKE_SOURCE_DIR}/runtime/guest/src/libblytc/blytc_stubs.c")

    set(LIBBLYTC_SRCS
        ${LIBBLYTC_STRING_SRCS}
        ${LIBBLYTC_MATH_SRCS}
        ${LIBBLYTC_CTYPE_SRCS}
        ${LIBBLYTC_STDLIB_SRCS}
        ${LIBBLYTC_INTERNAL_SRCS}
        ${LIBBLYTC_STDIO_SRCS}
        ${LIBBLYTC_OWN_SRCS})

    # Include paths for musl internal compilation. LIBBLYTC_BITS_DIR provides
    # the generated bits/alltypes.h.
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
        "${CMAKE_BINARY_DIR}/libblytc") # bits/alltypes.h lives here

    # Determinism and musl-compatibility compile flags.
    set(LIBBLYTC_CFLAGS
        -ffp-contract=off
        -fno-fast-math
        -fno-strict-aliasing
        -Wno-unused-parameter
        -Wno-sign-compare
        -Wno-implicit-fallthrough
        -Wno-unused-variable
        -Wno-deprecated-non-prototype)

    add_custom_command(
      OUTPUT "${LIBBLYTC_OUT}"
      COMMAND
        "${CLANG_RV32}" ${RV32_FLAGS} ${LIBBLYTC_INCLUDES} ${LIBBLYTC_CFLAGS}
        -Wl,-soname,libblytc.so -o "${LIBBLYTC_OUT}" ${LIBBLYTC_SRCS}
      DEPENDS ${LIBBLYTC_SRCS} "${LIBBLYTC_ALLTYPES_H}"
      COMMENT "Cross-compiling libblytc.so (RV32IMAFC, musl subset)"
      VERBATIM)

    message(STATUS "libblytc.so: will be built (musl at ${MUSL_DIR})")

    # ---- libblyt32lua.so (Lua VM + SoftFloat + runtime stubs) ----
    #
    # Provides the symbols that libblytc.so omits: strerror, soft-float double
    # ops (__muldf3 etc.), setjmp/longjmp, and the Lua VM itself. Embedded into
    # blyt_libretro.so alongside the other guest libs so the core is
    # self-contained (no BLYT_LIB_DIR required at runtime).
    set(LIBBLYT32LUA_OUT "")
    set(_LUA_DIR "${CMAKE_SOURCE_DIR}/third_party/lua")
    set(_SF_SRC "${CMAKE_SOURCE_DIR}/third_party/rv32emu/src/softfloat/source")
    if(EXISTS "${_LUA_DIR}/lvm.c" AND EXISTS "${_SF_SRC}/s_addMagsF64.c")
      set(LIBBLYT32LUA_OUT "${CMAKE_BINARY_DIR}/libblyt32lua.so")

      set(_SF_PLATFORM_DIR "${CMAKE_BINARY_DIR}/softfloat-rv32")
      file(MAKE_DIRECTORY "${_SF_PLATFORM_DIR}")
      file(WRITE "${_SF_PLATFORM_DIR}/platform.h" "#define THREAD_LOCAL\n")

      file(GLOB _SF_ALL "${_SF_SRC}/*.c")
      foreach(
        _EXCL_PAT
        "${_SF_SRC}/extF80*"
        "${_SF_SRC}/*M_*"
        "${_SF_SRC}/*M.*"
        "${_SF_SRC}/bf16*"
        "${_SF_SRC}/f16*"
        "${_SF_SRC}/f32_to_extF80*"
        "${_SF_SRC}/f64_to_extF80*"
        "${_SF_SRC}/f128_to_extF80*")
        file(GLOB _EXCL_LIST "${_EXCL_PAT}")
        list(REMOVE_ITEM _SF_ALL ${_EXCL_LIST})
      endforeach()
      file(
        GLOB
        _SF_RISCV
        "${_SF_SRC}/RISCV/s_propagateNaNF16UI.c"
        "${_SF_SRC}/RISCV/s_propagateNaNF32UI.c"
        "${_SF_SRC}/RISCV/s_propagateNaNF64UI.c"
        "${_SF_SRC}/RISCV/s_propagateNaNF128UI.c"
        "${_SF_SRC}/RISCV/s_propagateNaNExtF80UI.c"
        "${_SF_SRC}/RISCV/s_f16UIToCommonNaN.c"
        "${_SF_SRC}/RISCV/s_f32UIToCommonNaN.c"
        "${_SF_SRC}/RISCV/s_f64UIToCommonNaN.c"
        "${_SF_SRC}/RISCV/s_f128UIToCommonNaN.c"
        "${_SF_SRC}/RISCV/s_extF80UIToCommonNaN.c"
        "${_SF_SRC}/RISCV/s_commonNaNToF16UI.c"
        "${_SF_SRC}/RISCV/s_commonNaNToF32UI.c"
        "${_SF_SRC}/RISCV/s_commonNaNToF64UI.c"
        "${_SF_SRC}/RISCV/s_commonNaNToF128UI.c"
        "${_SF_SRC}/RISCV/s_commonNaNToExtF80UI.c"
        "${_SF_SRC}/RISCV/softfloat_raiseFlags.c")
      set(_SF_MWORD
          "${_SF_SRC}/s_add256M.c" "${_SF_SRC}/s_sub256M.c"
          "${_SF_SRC}/s_mul128To256M.c" "${_SF_SRC}/s_shiftRightJam256M.c")

      file(GLOB _LUA_SRCS "${_LUA_DIR}/*.c")
      foreach(
        _EXCL
        "${_LUA_DIR}/lua.c"
        "${_LUA_DIR}/luac.c"
        "${_LUA_DIR}/onelua.c"
        "${_LUA_DIR}/liolib.c"
        "${_LUA_DIR}/loslib.c"
        "${_LUA_DIR}/loadlib.c"
        "${_LUA_DIR}/ldblib.c"
        "${_LUA_DIR}/lutf8lib.c")
        list(REMOVE_ITEM _LUA_SRCS "${_EXCL}")
      endforeach()

      # Public musl headers — avoids the `#define weak __attribute__((weak))` in
      # musl's src/include/ that clashes with Lua's GCObject *weak field.
      set(_LUA_MUSL_INC
          -I
          "${MUSL_DIR}/include"
          -I
          "${MUSL_DIR}/arch/riscv32"
          -I
          "${MUSL_DIR}/arch/generic"
          -I
          "${LIBBLYTC_BITS_DIR}/..")

      set(_LUA32_SRCS
          ${_LUA_SRCS}
          "${MUSL_DIR}/src/setjmp/riscv32/setjmp.S"
          "${MUSL_DIR}/src/setjmp/riscv32/longjmp.S"
          ${_SF_ALL}
          ${_SF_RISCV}
          ${_SF_MWORD}
          "${CMAKE_SOURCE_DIR}/runtime/guest/src/libblyt32lua/softfloat_builtins.c"
          "${CMAKE_SOURCE_DIR}/runtime/guest/src/libblyt32lua/lua_runtime_stubs.c"
          "${CMAKE_SOURCE_DIR}/runtime/guest/src/libblyt32lua/blyt32lua.c"
          "${CMAKE_SOURCE_DIR}/runtime/host/src/dap/master_hook.c"
          "${CMAKE_SOURCE_DIR}/runtime/guest/src/libblyt32lua/master_hook_ecall.c"
      )

      add_custom_command(
        OUTPUT "${LIBBLYT32LUA_OUT}"
        COMMAND
          "${CLANG_RV32}" ${RV32_FLAGS} ${RV32_INCLUDE} ${_LUA_MUSL_INC}
          ${LIBBLYTC_CFLAGS} -DLUA_32BITS=1 -DLUA_USE_LONGJMP=1 -DBLYT_DAP=1 -I
          "${_LUA_DIR}" -I "${_SF_SRC}/include" -I "${_SF_SRC}/RISCV" -I
          "${_SF_PLATFORM_DIR}" -I "${CMAKE_SOURCE_DIR}/runtime/host/src/dap"
          -DSOFTFLOAT_FAST_INT64=1 -DSOFTFLOAT_ROUND_ODD=1
          -Wl,-soname,libblyt32lua.so -Wl,--as-needed "${LIBBLYT32_OUT}" -o
          "${LIBBLYT32LUA_OUT}" ${_LUA32_SRCS}
        DEPENDS "${LIBBLYT32_OUT}" ${_LUA32_SRCS}
        COMMENT "Cross-compiling libblyt32lua.so (RV32IMAFC, Lua + SoftFloat)"
        VERBATIM)

      message(STATUS "libblyt32lua.so: will be built (Lua + SoftFloat)")
    else()
      message(STATUS "libblyt32lua.so: skipping \
(third_party/lua or rv32emu/softfloat not initialised)")
    endif()
  endif()

  # libblyt32.so — Blyt32 variant. DT_NEEDED: libblytcommon.so, libblytc.so (the
  # latter via --no-as-needed so the entry is added even though libblyt32.so's
  # own code doesn't reference libblytc symbols directly).
  set(LIBBLYT32_SRC "${CMAKE_SOURCE_DIR}/runtime/guest/src/libblyt32/blyt32.c")
  set(LIBBLYT32_OUT "${CMAKE_BINARY_DIR}/libblyt32.so")

  if(MUSL_DIR)
    # Full build: DT_NEEDED for both libblytcommon.so and libblytc.so. Use full
    # paths to avoid lld defaulting to -Bstatic on bare-metal targets.
    set(LIBBLYT32_DEPS "${LIBBLYT32_SRC}" "${BLYT_H}" "${LIBBLYTCOMMON_OUT}"
                       "${LIBBLYTC_OUT}")
    set(LIBBLYT32_LINK -Wl,-Bdynamic "${LIBBLYTCOMMON_OUT}" -Wl,--no-as-needed
                       "${LIBBLYTC_OUT}" -Wl,--as-needed)
  else()
    # musl unavailable: build without libblytc.so dependency.
    set(LIBBLYT32_DEPS "${LIBBLYT32_SRC}" "${BLYT_H}" "${LIBBLYTCOMMON_OUT}")
    set(LIBBLYT32_LINK -Wl,-Bdynamic "${LIBBLYTCOMMON_OUT}")
  endif()

  add_custom_command(
    OUTPUT "${LIBBLYT32_OUT}"
    COMMAND
      "${CLANG_RV32}" ${RV32_FLAGS} -Wl,-soname,libblyt32.so ${RV32_INCLUDE}
      ${LIBBLYT32_LINK} -o "${LIBBLYT32_OUT}" "${LIBBLYT32_SRC}"
    DEPENDS ${LIBBLYT32_DEPS}
    COMMENT "Cross-compiling libblyt32.so (RV32IMAFC)"
    VERBATIM)

  if(MUSL_DIR)
    set(_libblyt32_all_deps "${LIBBLYT32_OUT}" "${LIBBLYTCOMMON_OUT}"
                            "${LIBBLYTC_OUT}")
    if(LIBBLYT32LUA_OUT)
      list(APPEND _libblyt32_all_deps "${LIBBLYT32LUA_OUT}")
    endif()
    add_custom_target(libblyt32_so ALL DEPENDS ${_libblyt32_all_deps})
    message(
      STATUS "libblytcommon.so + libblytc.so + libblyt32.so: will be built \
(${CLANG_RV32})")
  else()
    add_custom_target(libblyt32_so ALL DEPENDS "${LIBBLYT32_OUT}"
                                               "${LIBBLYTCOMMON_OUT}")
    message(
      STATUS "libblytcommon.so + libblyt32.so: will be built (${CLANG_RV32})")
  endif()

  # -------------------------------------------------------------------------
  # Native-path libblyt32.so (frontends/native/src/libblyt32/)
  #
  # Real API implementations for trusted native exec on RISC-V:
  # blyt_console_debug → write(2,...), seccomp constructor, and future API
  # functions. Placed in build/native/ alongside libblytcommon.so + libblytc.so
  # so the launcher can use a single LD_LIBRARY_PATH=build/native.
  # -------------------------------------------------------------------------
  set(LIBBLYT32_NATIVE_SRC
      "${CMAKE_SOURCE_DIR}/frontends/native/src/libblyt32/blyt32.c")
  set(LIBBLYT32_NATIVE_INC "${CMAKE_SOURCE_DIR}/frontends/native/src/libblyt32")
  set(LIBBLYT32_NATIVE_OUT "${CMAKE_BINARY_DIR}/native/libblyt32.so")
  file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/native")

  set(LIBBLYT32_NATIVE_DEPS
      "${LIBBLYT32_NATIVE_SRC}" "${LIBBLYT32_NATIVE_INC}/seccomp_restricted.h"
      "${BLYT_H}" "${LIBBLYTCOMMON_OUT}")

  # Native link flags: --no-as-needed forces libblytcommon.so into DT_NEEDED.
  # Without it, --as-needed (the default) would silently drop libblytcommon.so
  # because native blyt32.c uses no symbols from it directly; blyt_main and
  # other libblytcommon exports would then be unresolvable at cart load time,
  # causing a crash before any constructor fires.  libblytc.so is intentionally
  # omitted — the native process C library comes from the musl ld.so
  # interpreter.  Use full path to avoid lld -Bstatic on bare-metal targets.
  set(LIBBLYT32_NATIVE_LINK -Wl,-Bdynamic -Wl,--no-as-needed
                            "${LIBBLYTCOMMON_OUT}" -Wl,--as-needed)

  add_custom_command(
    OUTPUT "${LIBBLYT32_NATIVE_OUT}"
    COMMAND
      "${CLANG_RV32}" ${RV32_FLAGS} -Wl,-soname,libblyt32.so ${RV32_INCLUDE} -I
      "${LIBBLYT32_NATIVE_INC}" ${LIBBLYT32_NATIVE_LINK} -o
      "${LIBBLYT32_NATIVE_OUT}" "${LIBBLYT32_NATIVE_SRC}"
    DEPENDS ${LIBBLYT32_NATIVE_DEPS}
    COMMENT "Cross-compiling libblyt32.so (RV32IMAFC, native build)"
    VERBATIM)

  # Stage libblytcommon.so alongside native libblyt32.so. libblytc.so is
  # intentionally NOT staged: the native cart process gets its C library from
  # the musl ld.so interpreter; staging libblytc.so (our musl subset) would load
  # a second musl instance, conflicting with the interpreter.
  add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/native/libblytcommon.so"
    COMMAND "${CMAKE_COMMAND}" -E copy "${LIBBLYTCOMMON_OUT}"
            "${CMAKE_BINARY_DIR}/native/libblytcommon.so"
    DEPENDS "${LIBBLYTCOMMON_OUT}"
    COMMENT "Staging libblytcommon.so to native/"
    VERBATIM)

  add_custom_target(
    libblyt32_native_so ALL
    DEPENDS "${LIBBLYT32_NATIVE_OUT}"
            "${CMAKE_BINARY_DIR}/native/libblytcommon.so")
  message(STATUS "libblyt32.so (native build): will be built → build/native/")

else()
  message(STATUS "libblytcommon.so + libblyt32.so + libblytc.so: skipping \
(need clang riscv32 + ld.lld)")
endif()

# -------------------------------------------------------------------------
# Host runtime libraries (stubs for future expansion; only libblyt is real)
# -------------------------------------------------------------------------
add_library(libblytcommon INTERFACE)
add_library(libblytcommonlua INTERFACE)
add_library(libblyt32lua INTERFACE)
add_library(libblytc INTERFACE)

add_library(
  libblyt STATIC
  runtime/host/src/libblyt/cart_load.c runtime/host/src/libblyt/cart_run.c
  runtime/host/src/libblyt/testcard.c)
set_target_properties(libblyt PROPERTIES OUTPUT_NAME blyt)
# blyt_libretro.so (a shared MODULE) links libblyt.a; compile with -fPIC so the
# same static library is usable in both executables and shared objects.
set_target_properties(libblyt libblytemu libsoftfloat
                      PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(
  libblyt
  PUBLIC runtime/host/include
  PRIVATE runtime/host/src/libblyt third_party/flatcc/include
          "${GENERATED_DIR}" ${RV32EMU_DIR})
# cart_run.c includes common.h explicitly; the -D flags ensure feature macros
# expand correctly when rv32emu headers are pulled in from cart_run.c.
target_compile_definitions(libblyt PRIVATE ${RV32EMU_FEATURE_DEFS})
target_link_libraries(
  libblyt
  PUBLIC flatccrt
  PRIVATE libblytemu)
add_dependencies(libblyt flatbuffers_generated)

# -------------------------------------------------------------------------
# DAP debugging support (TCP+pthread transport for SDL2 / libretro)
#
# dap is a static library containing dap_server.c (TCP transport). Linked into
# libblyt and blytplay when BLYT_DAP=ON. master_hook.c is compiled into the WASM
# binary separately (frontends/wasm/) and into libblyt32lua.so as RV32 guest
# code (cmake/blyt_sdk.cmake). It is NOT part of the native host libdap.a — the
# master hook runs inside rv32emu and does not need to be present in the host
# address space.
# -------------------------------------------------------------------------
option(BLYT_DAP "Enable DAP Lua debugger support (TCP transport)" ON)

if(BLYT_DAP)
  set(BLYT_DAP_DIR "${CMAKE_SOURCE_DIR}/runtime/host/src/dap")
  add_library(dap STATIC "${BLYT_DAP_DIR}/dap_server.c")
  set_target_properties(dap PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_include_directories(
    dap
    PUBLIC "${BLYT_DAP_DIR}"
    PRIVATE "${CMAKE_SOURCE_DIR}/runtime/host/include")
  target_compile_definitions(dap PRIVATE BLYT_DAP=1)

  target_compile_definitions(libblyt PRIVATE BLYT_DAP=1)
  target_include_directories(libblyt PRIVATE "${BLYT_DAP_DIR}")
  target_link_libraries(libblyt PRIVATE dap)
  message(STATUS "BLYT_DAP: DAP debugging enabled (TCP transport)")
else()
  message(STATUS "BLYT_DAP: DAP debugging disabled")
endif()

# -------------------------------------------------------------------------
# GDB RSP debugging support (TCP+pthread transport for SDL2 / libretro)
#
# gdb_stub is a static library containing gdb_stub.c (protocol handler, no
# threading) and gdb_transport_tcp.c (blocking TCP accept + pthread reader).
# Linked into libblyt when BLYT_GDB=ON. gdb_transport_wasm.c is compiled into
# the WASM binary separately (frontends/wasm/CMakeLists.txt).
# -------------------------------------------------------------------------
find_package(Threads QUIET)
option(BLYT_GDB "Enable GDB RSP debugger support (TCP transport)" ON)

if(BLYT_GDB AND Threads_FOUND)
  set(BLYT_GDB_DIR "${CMAKE_SOURCE_DIR}/runtime/host/src/gdb")
  add_library(gdb_stub STATIC "${BLYT_GDB_DIR}/gdb_stub.c"
                              "${BLYT_GDB_DIR}/gdb_transport_tcp.c")
  set_target_properties(gdb_stub PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_include_directories(
    gdb_stub PRIVATE "${BLYT_GDB_DIR}"
                     "${CMAKE_SOURCE_DIR}/runtime/host/include")
  target_compile_definitions(gdb_stub PRIVATE BLYT_GDB=1 BLYT_GDB_TCP=1)
  target_link_libraries(gdb_stub PRIVATE Threads::Threads)

  target_compile_definitions(libblyt PRIVATE BLYT_GDB=1)
  target_include_directories(libblyt PRIVATE "${BLYT_GDB_DIR}")
  target_link_libraries(libblyt PRIVATE gdb_stub)
  message(STATUS "BLYT_GDB: GDB RSP debugging enabled (TCP transport)")
elseif(BLYT_GDB)
  message(STATUS "BLYT_GDB: disabled (Threads not found)")
else()
  message(STATUS "BLYT_GDB: GDB RSP debugging disabled")
endif()

# -------------------------------------------------------------------------
# SDL2 frontend — try find_package, then pkg-config, then sdl2-config
# -------------------------------------------------------------------------
find_package(SDL2 QUIET)
if(SDL2_FOUND OR TARGET SDL2::SDL2)
  add_executable(blytplay frontends/player/main.c)
  target_link_libraries(blytplay PRIVATE libblyt SDL2::SDL2)
  message(STATUS "SDL2 found via find_package — blytplay (SDL2) will be built")
else()
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(SDL2 QUIET sdl2)
  endif()
  if(SDL2_FOUND)
    add_executable(blytplay frontends/player/main.c)
    target_include_directories(blytplay PRIVATE ${SDL2_INCLUDE_DIRS})
    target_compile_options(blytplay PRIVATE ${SDL2_CFLAGS_OTHER})
    target_link_libraries(blytplay PRIVATE libblyt ${SDL2_LIBRARIES})
    message(STATUS "SDL2 found via pkg-config — blytplay (SDL2) will be built")
  else()
    # Last resort: try sdl2-config
    find_program(SDL2_CONFIG sdl2-config)
    if(SDL2_CONFIG)
      execute_process(
        COMMAND ${SDL2_CONFIG} --cflags
        OUTPUT_VARIABLE SDL2_CFLAGS
        OUTPUT_STRIP_TRAILING_WHITESPACE)
      execute_process(
        COMMAND ${SDL2_CONFIG} --libs
        OUTPUT_VARIABLE SDL2_LDFLAGS
        OUTPUT_STRIP_TRAILING_WHITESPACE)
      separate_arguments(SDL2_CFLAGS_LIST UNIX_COMMAND "${SDL2_CFLAGS}")
      separate_arguments(SDL2_LDFLAGS_LIST UNIX_COMMAND "${SDL2_LDFLAGS}")
      add_executable(blytplay frontends/player/main.c)
      target_compile_options(blytplay PRIVATE ${SDL2_CFLAGS_LIST})
      target_link_options(blytplay PRIVATE ${SDL2_LDFLAGS_LIST})
      target_link_libraries(blytplay PRIVATE libblyt)
      message(
        STATUS "SDL2 found via sdl2-config — blytplay (SDL2) will be built")
    else()
      message(STATUS "SDL2 not found — blytplay (SDL2) will not be built")
    endif()
  endif()
endif()

# blytplay drives the runtime via the libretro API (blyt_libretro.c compiled in
# directly, without BLYT_EMBED_LIBS so dynlink uses BLYT_LIB_DIR as usual).
if(TARGET blytplay)
  target_sources(blytplay PRIVATE frontends/libretro/blyt_libretro.c)
  target_include_directories(
    blytplay PRIVATE "${CMAKE_SOURCE_DIR}/third_party/libretro-common/include"
                     "${CMAKE_SOURCE_DIR}/runtime/host/include")
  target_compile_definitions(blytplay PRIVATE BLYT_VERSION="${BLYT_VERSION}")
  if(BLYT_DAP)
    target_compile_definitions(blytplay PRIVATE BLYT_DAP=1)
  endif()
  if(BLYT_GDB AND Threads_FOUND)
    target_compile_definitions(blytplay PRIVATE BLYT_GDB=1)
  endif()
endif()

# -------------------------------------------------------------------------
# blyt_libretro — libretro core
#
# Embeds libblytcommon.so, libblytc.so, and libblyt32.so as compiled-in data so
# the core is self-contained.  Only built when all three guest libs are
# available (clang + lld + musl all found above).
# -------------------------------------------------------------------------
if(LIBBLYTC_OUT)
  set(EMBEDDED_LIBS_C "${CMAKE_BINARY_DIR}/blyt_embedded_libs.c")
  set(_bin2c_args blytcommon_so "${LIBBLYTCOMMON_OUT}" blytc_so
                  "${LIBBLYTC_OUT}" blyt32_so "${LIBBLYT32_OUT}")
  set(_bin2c_deps "${LIBBLYTCOMMON_OUT}" "${LIBBLYTC_OUT}" "${LIBBLYT32_OUT}")
  if(LIBBLYT32LUA_OUT)
    list(APPEND _bin2c_args blyt32lua_so "${LIBBLYT32LUA_OUT}")
    list(APPEND _bin2c_deps "${LIBBLYT32LUA_OUT}")
  endif()
  add_custom_command(
    OUTPUT "${EMBEDDED_LIBS_C}"
    COMMAND python3 "${CMAKE_SOURCE_DIR}/cmake/bin2c.py" ${_bin2c_args}
            "${EMBEDDED_LIBS_C}"
    DEPENDS ${_bin2c_deps}
    COMMENT "Embedding guest libs into blyt_libretro"
    VERBATIM)

  add_library(blyt_libretro MODULE frontends/libretro/blyt_libretro.c
                                   "${EMBEDDED_LIBS_C}")
  target_link_libraries(blyt_libretro PRIVATE libblyt)
  target_include_directories(
    blyt_libretro
    PRIVATE "${CMAKE_SOURCE_DIR}/third_party/libretro-common/include"
            "${CMAKE_SOURCE_DIR}/runtime/host/include")
  set(_blyt_libretro_defs BLYT_VERSION="${BLYT_VERSION}" BLYT_EMBED_LIBS)
  if(LIBBLYT32LUA_OUT)
    list(APPEND _blyt_libretro_defs BLYT_EMBED_LUA)
  endif()
  if(BLYT_DAP)
    list(APPEND _blyt_libretro_defs BLYT_DAP=1)
  endif()
  if(BLYT_GDB AND Threads_FOUND)
    list(APPEND _blyt_libretro_defs BLYT_GDB=1)
  endif()
  target_compile_definitions(blyt_libretro PRIVATE ${_blyt_libretro_defs})
  set_target_properties(blyt_libretro PROPERTIES PREFIX "" SUFFIX ".so")
  add_dependencies(blyt_libretro libblyt32_so)
  message(STATUS "blyt_libretro: will be built")
else()
  add_library(blyt_libretro INTERFACE)
  message(STATUS "blyt_libretro: skipping (need clang riscv32 + lld + musl)")
endif()

# -------------------------------------------------------------------------
# blyt_native — LP64 RISC-V launcher for trusted native cart execution
#
# Installs the arch-dispatch seccomp filter then execs the ILP32 cart. Only
# built on Linux RISC-V 64-bit (the target host for native execution).
# -------------------------------------------------------------------------
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES
                                          "riscv64")
  add_executable(blyt_native frontends/native/launcher.c)
  target_include_directories(blyt_native PRIVATE frontends/native
                                                 runtime/host/include)
  target_compile_definitions(blyt_native PRIVATE BLYT_VERSION="${BLYT_VERSION}"
                                                 BLYT_WITH_CART_VALIDATE)
  target_link_libraries(blyt_native PRIVATE libblyt)
  # Cross-compilation: link statically so the binary runs on any riscv64 Linux
  # regardless of which C library (glibc, musl, …) the target system uses.
  if(CMAKE_CROSSCOMPILING)
    target_link_options(blyt_native PRIVATE -static)
  endif()
  message(STATUS "blyt_native (LP64 launcher): will be built")
else()
  message(
    STATUS "blyt_native (LP64 launcher): skipping (requires Linux riscv64)")
endif()

# -------------------------------------------------------------------------
# blyt_native_riscv64 — convenience target: cross-compile blyt_native for
# riscv64-linux from the host (macOS or non-riscv64 Linux).
#
# On macOS: runs inside an ubuntu:24.04 Docker container where
# gcc-riscv64-linux-gnu is available.  Docker must be running. On Linux
# (non-riscv64): runs cmake directly, relying on gcc-riscv64-linux-gnu being
# installed (e.g. sudo apt install gcc-riscv64-linux-gnu).
#
# Output: ${CMAKE_BINARY_DIR}/../build-riscv64/blyt_native
# -------------------------------------------------------------------------
if(NOT CMAKE_CROSSCOMPILING
   AND NOT (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR
                                                   MATCHES "riscv64"))
  set(_riscv64_build_dir "${CMAKE_SOURCE_DIR}/build-riscv64")
  set(_toolchain "${CMAKE_SOURCE_DIR}/cmake/riscv64-linux-gnu.cmake")
  set(_pregen_dir "${CMAKE_BINARY_DIR}/generated/flatbuffers")
  set(_cmake_cmd
      ${CMAKE_COMMAND}
      -B
      "${_riscv64_build_dir}"
      -G
      Ninja
      -DCMAKE_TOOLCHAIN_FILE=${_toolchain}
      -DBLYT_PREGENERATED_DIR=${_pregen_dir}
      COMMAND
      ${CMAKE_COMMAND}
      --build
      "${_riscv64_build_dir}"
      --target
      blyt_native)

  if(APPLE)
    find_program(DOCKER_EXECUTABLE docker HINTS /usr/local/bin
                                                /opt/homebrew/bin NO_CACHE)
    if(DOCKER_EXECUTABLE)
      # Docker: install gcc-riscv64-linux-gnu in ubuntu:24.04 and cross-compile.
      # The cmake invocations are passed as separate arguments to avoid shell
      # quoting issues with inline heredocs.
      add_custom_target(
        blyt_native_riscv64
        COMMAND
          ${DOCKER_EXECUTABLE} run --platform linux/arm64 --rm --env
          DEBIAN_FRONTEND=noninteractive -v "${CMAKE_SOURCE_DIR}:/repo"
          ubuntu:24.04 bash -c
          [=[apt-get update -q 2>/dev/null && apt-get install -y --no-install-recommends cmake ninja-build gcc-riscv64-linux-gnu binutils-riscv64-linux-gnu libc6-dev-riscv64-cross libgcc-s1-riscv64-cross 2>&1 | tail -2 && cd /repo && cmake -B build-riscv64 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/riscv64-linux-gnu.cmake -DBLYT_PREGENERATED_DIR=build/generated/flatbuffers && cmake --build build-riscv64 --target blyt_native && chown -R $(id -u):$(id -g) build-riscv64 2>/dev/null || true]=]
        COMMENT "Cross-compiling blyt_native for riscv64-linux (via Docker)"
        USES_TERMINAL VERBATIM)
    else()
      add_custom_target(
        blyt_native_riscv64
        COMMAND
          ${CMAKE_COMMAND} -E echo
          "Docker not found — install Docker Desktop to cross-compile blyt_native"
        COMMENT "blyt_native_riscv64 requires Docker on macOS")
    endif()
  else()
    # Non-riscv64 Linux: gcc-riscv64-linux-gnu expected on PATH.
    add_custom_target(
      blyt_native_riscv64
      COMMAND ${_cmake_cmd}
      COMMENT "Cross-compiling blyt_native for riscv64-linux"
      USES_TERMINAL VERBATIM)
  endif()
endif()

# -------------------------------------------------------------------------
# Tests
# -------------------------------------------------------------------------
enable_testing()

# Test-only FlatBuffers builder headers (runtime only generates readers) Skipped
# when cross-compiling (blyt_native doesn't need them, and the test binaries
# won't run on the host when targeting a different architecture).
if(NOT DEFINED BLYT_PREGENERATED_DIR)
  set(TEST_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated/test")
  file(MAKE_DIRECTORY "${TEST_GENERATED_DIR}")

  add_custom_command(
    OUTPUT "${TEST_GENERATED_DIR}/cart_info_builder.h"
           "${TEST_GENERATED_DIR}/cart_config_builder.h"
           "${TEST_GENERATED_DIR}/flatbuffers_common_builder.h"
           "${TEST_GENERATED_DIR}/flatbuffers_common_reader.h"
           "${TEST_GENERATED_DIR}/cart_info_reader.h"
           "${TEST_GENERATED_DIR}/cart_config_reader.h"
    COMMAND
      "$<TARGET_FILE:flatcc_cli>" --common --reader --builder -o
      "${TEST_GENERATED_DIR}" "${SCHEMAS_DIR}/cart_info.fbs"
      "${SCHEMAS_DIR}/cart_config.fbs"
    DEPENDS flatcc_cli "${SCHEMAS_DIR}/cart_info.fbs"
            "${SCHEMAS_DIR}/cart_config.fbs"
    COMMENT "Generating FlatBuffers test builder headers"
    VERBATIM)

  add_custom_target(
    flatbuffers_test_generated
    DEPENDS "${TEST_GENERATED_DIR}/cart_info_builder.h"
            "${TEST_GENERATED_DIR}/cart_config_builder.h")
else()
  add_custom_target(flatbuffers_test_generated)
endif()

add_executable(test_cart_load tests/unit/test_cart_load.c)
target_include_directories(
  test_cart_load PRIVATE runtime/host/src/libblyt third_party/flatcc/include
                         "${TEST_GENERATED_DIR}")
target_link_libraries(test_cart_load PRIVATE libblyt flatccrt)
add_dependencies(test_cart_load flatbuffers_test_generated)
add_test(NAME test_cart_load COMMAND test_cart_load)

# test_cart_run: exercises the ECALL dispatch mechanism using rv32emu directly.
# The test includes common.h explicitly (first), then rv32emu headers; we pass
# the same feature definitions that libblytemu uses.
add_executable(test_cart_run tests/unit/test_cart_run.c)
target_include_directories(test_cart_run PRIVATE runtime/host/src/libblyt
                                                 ${RV32EMU_DIR})
target_compile_definitions(test_cart_run PRIVATE ${RV32EMU_FEATURE_DEFS})
target_link_libraries(test_cart_run PRIVATE libblytemu)
add_test(NAME test_cart_run COMMAND test_cart_run)

# test_gdb_stub: unit-tests for the GDB RSP protocol stub in isolation. Compiles
# gdb_stub.c directly without gdb_transport_tcp.c/pthreads so a mock transport
# can be injected cleanly.  Gated on BLYT_GDB so it is only built when GDB
# support is compiled in.
if(BLYT_GDB)
  add_executable(test_gdb_stub tests/unit/test_gdb_stub.c
                               runtime/host/src/gdb/gdb_stub.c)
  target_include_directories(test_gdb_stub PRIVATE runtime/host/src/gdb
                                                   runtime/host/include)
  # BLYT_GDB enables stub code; BLYT_GDB_TEST exposes fc_gdb_stub_test_reset().
  # No BLYT_GDB_TCP: avoids pthreads so the test is self-contained.
  target_compile_definitions(test_gdb_stub PRIVATE BLYT_GDB=1 BLYT_GDB_TEST=1)
  add_test(NAME test_gdb_stub COMMAND test_gdb_stub)
endif()

# verify_cart: standalone tool used in CI to confirm blyt build output passes
# load-time security checks. Not a ctest test; invoked directly with a cart.
add_executable(verify_cart tests/tools/verify_cart.c)
target_link_libraries(verify_cart PRIVATE libblyt)

# test_session_api: exercises blyt_session_t and blyt_register_lib directly. Not
# a ctest test (requires the SDK to be assembled first for the guest libs and a
# real cart); invoked from Rust integration tests after `cmake --build build
# --target sdk`.
add_executable(test_session_api tests/unit/test_session_api.c)
target_include_directories(test_session_api PRIVATE runtime/host/include)
target_link_libraries(test_session_api PRIVATE libblyt)

# test_libretro_dap: dlopens blyt_libretro.so and verifies the DAP TCP server
# starts and responds to an "initialize" request.  Built only when the libretro
# core is available (LIBBLYTC_OUT set → blyt_libretro.so will be built).
if(LIBBLYTC_OUT)
  add_executable(test_libretro_dap tests/unit/test_libretro_dap.c)
  target_include_directories(test_libretro_dap
                             PRIVATE third_party/libretro-common/include)
  target_link_libraries(test_libretro_dap PRIVATE ${CMAKE_DL_LIBS})

  add_executable(test_libretro_core tests/unit/test_libretro_core.c)
  target_include_directories(test_libretro_core
                             PRIVATE third_party/libretro-common/include)
  target_link_libraries(test_libretro_core PRIVATE ${CMAKE_DL_LIBS})
endif()

# -------------------------------------------------------------------------
# SDK assembly target
#
# cmake --build build --target sdk
#
# Downloads a pre-built LLVM toolchain if no riscv32-capable clang is found,
# builds libblytcommon.so + libblyt32.so (RV32IMAFC), copies the devtool and
# headers, and assembles a self-contained SDK under build/sdk/.
#
# After building, carts can be built with: build/sdk/bin/blyt build
# <project-dir>
# -------------------------------------------------------------------------
add_custom_target(
  sdk
  COMMAND
    ${CMAKE_COMMAND} -DBLYT_SOURCE_DIR=${CMAKE_SOURCE_DIR}
    -DBLYT_BINARY_DIR=${CMAKE_BINARY_DIR} -DBLYT_VERSION=${BLYT_VERSION}
    -DCMAKE_HOST_SYSTEM_NAME=${CMAKE_HOST_SYSTEM_NAME}
    -DCMAKE_HOST_SYSTEM_PROCESSOR=${CMAKE_HOST_SYSTEM_PROCESSOR} -P
    ${CMAKE_SOURCE_DIR}/cmake/blyt_sdk.cmake
  USES_TERMINAL
  COMMENT "Assembling blyt SDK in build/sdk/"
  VERBATIM)

# -------------------------------------------------------------------------
# macOS: generate .dSYM bundles after linking so LLDB can locate source for code
# in static libraries (rv32emu, flatcc).  cmake+Ninja does not run dsymutil
# automatically; without .dSYM bundles, LLDB falls back to N_OSO stabs which are
# less reliable and can leave source files unresolvable.
# -------------------------------------------------------------------------
# -------------------------------------------------------------------------
# VS Code extension: install-vscode-ext target for local development. Creates a
# symlink in ~/.vscode/extensions/ so edits to tools/vscode/ are reflected
# immediately on VS Code window reload.
# -------------------------------------------------------------------------
file(STRINGS "${CMAKE_SOURCE_DIR}/tools/vscode/package.json" _vscode_pkg_lines)
foreach(_line IN LISTS _vscode_pkg_lines)
  if(_line MATCHES "^[ \t]*\"publisher\":[ \t]*\"([^\"]+)\"")
    set(_vscode_publisher "${CMAKE_MATCH_1}")
  elseif(_line MATCHES "^[ \t]*\"name\":[ \t]*\"([^\"]+)\"")
    set(_vscode_name "${CMAKE_MATCH_1}")
  elseif(_line MATCHES "^[ \t]*\"version\":[ \t]*\"([^\"]+)\"")
    set(_vscode_version "${CMAKE_MATCH_1}")
  endif()
endforeach()
set(_vscode_ext_dir
    "$ENV{HOME}/.vscode/extensions/${_vscode_publisher}.${_vscode_name}-${_vscode_version}"
)

add_custom_target(
  install-vscode-ext
  COMMAND ${CMAKE_COMMAND} -E rm -rf "${_vscode_ext_dir}"
  COMMAND ${CMAKE_COMMAND} -E create_symlink "${CMAKE_SOURCE_DIR}/tools/vscode"
          "${_vscode_ext_dir}"
  COMMENT "Installing VS Code extension → ${_vscode_ext_dir}"
  VERBATIM)

if(APPLE)
  find_program(DSYMUTIL dsymutil)
  if(DSYMUTIL)
    foreach(_bin blytplay test_cart_load test_cart_run verify_cart)
      if(TARGET ${_bin})
        add_custom_command(
          TARGET ${_bin}
          POST_BUILD
          COMMAND "${DSYMUTIL}" "$<TARGET_FILE:${_bin}>"
          COMMENT "dsymutil ${_bin}"
          VERBATIM)
      endif()
    endforeach()
  endif()
endif()

# ── QEMU images
# ──────────────────────────────────────────────────────────────── kernel +
# rootfs.qcow2 + id_ed25519 are downloaded from a GitHub release into
# qemu-images/.  Override the release base URL with -DBLYT_QEMU_IMAGE_URL=...
set(BLYT_QEMU_IMAGE_URL
    "https://github.com/blyt-tech/blyt-qemu-images/releases/download/v2"
    CACHE STRING "Base URL for QEMU image downloads")

add_custom_target(
  fetch_qemu_images
  COMMAND
    ${CMAKE_COMMAND} -DQEMU_IMAGES_DIR=${CMAKE_SOURCE_DIR}/qemu-images
    -DBLYT_QEMU_IMAGE_URL=${BLYT_QEMU_IMAGE_URL} -P
    ${CMAKE_SOURCE_DIR}/cmake/fetch_qemu_images.cmake
  COMMENT "Downloading QEMU images → qemu-images/"
  VERBATIM)

# ── lint ──────────────────────────────────────────────────────────────────────
# Mirrors the "Check formatting" step in .github/workflows/ci.yml exactly. Run
# before every commit: cmake --build build --target lint

file(
  GLOB
  _LINT_C_FILES
  "${CMAKE_SOURCE_DIR}/runtime/host/src/libblyt/*.h"
  "${CMAKE_SOURCE_DIR}/runtime/host/src/libblyt/*.c"
  "${CMAKE_SOURCE_DIR}/runtime/host/src/gdb/*.h"
  "${CMAKE_SOURCE_DIR}/runtime/host/src/gdb/*.c"
  "${CMAKE_SOURCE_DIR}/runtime/guest/src/libblytcommon/*.c"
  "${CMAKE_SOURCE_DIR}/runtime/guest/src/libblyt32/*.c"
  "${CMAKE_SOURCE_DIR}/runtime/guest/src/libblytc/blytc_arena.c"
  "${CMAKE_SOURCE_DIR}/runtime/guest/src/libblytc/blytc_stubs.c"
  "${CMAKE_SOURCE_DIR}/runtime/guest/include/*.h"
  "${CMAKE_SOURCE_DIR}/runtime/host/include/*.h"
  "${CMAKE_SOURCE_DIR}/tests/unit/*.c"
  "${CMAKE_SOURCE_DIR}/tests/tools/*.c"
  "${CMAKE_SOURCE_DIR}/tests/game/hello-c/src/game/c/*.c"
  "${CMAKE_SOURCE_DIR}/frontends/player/main.c"
  "${CMAKE_SOURCE_DIR}/frontends/libretro/blyt_libretro.c"
  "${CMAKE_SOURCE_DIR}/frontends/wasm/wasm_main.c"
  "${CMAKE_SOURCE_DIR}/frontends/native/launcher.c"
  "${CMAKE_SOURCE_DIR}/frontends/native/src/libblyt32/blyt32.c"
  "${CMAKE_SOURCE_DIR}/runtime/host/src/libblyt/testcard.c")

add_custom_target(
  lint
  COMMAND clang-format --dry-run --Werror ${_LINT_C_FILES}
  COMMAND cmake-format --check "${CMAKE_SOURCE_DIR}/CMakeLists.txt"
          "${CMAKE_SOURCE_DIR}/frontends/wasm/CMakeLists.txt"
  COMMAND yamllint "${CMAKE_SOURCE_DIR}/.github/workflows/ci.yml"
  COMMAND cargo fmt --check --all
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  COMMENT "Linting: clang-format, cmake-format, yamllint, cargo fmt"
  VERBATIM)

# ── test-linux-docker ─────────────────────────────────────────────────────────
# Run integration tests inside a Linux Docker container that mirrors the GitHub
# CI environment (ubuntu-24.04 + LLVM 22 + Rust + emsdk).
#
# Usage: cmake --build build --target test-linux-docker
#
# To run a single test, reconfigure with a filter first: cmake
# -DBLYT_LINUX_TEST_FILTER=sdl_hybrid_gdb_and_dap -B build cmake --build build
# --target test-linux-docker
#
# To test arm64, pass -DBLYT_LINUX_ARCH=arm64. Each arch keeps its own named
# Docker image and build volumes so switching architectures does not require a
# full rebuild of the other.
#
# Delete volumes with: docker volume rm blyt-linux-build-amd64
set(BLYT_LINUX_TEST_FILTER
    ""
    CACHE
      STRING
      "Cargo test filter for test-linux-docker (e.g. sdl_hybrid_gdb_and_dap)")
set(BLYT_LINUX_ARCH
    "amd64"
    CACHE STRING
          "Docker target architecture for test-linux-docker (amd64 or arm64)")
find_program(
  _BLYT_DOCKER docker
  HINTS /usr/local/bin /opt/homebrew/bin /usr/bin NO_CACHE
  NO_DEFAULT_PATH)
if(_BLYT_DOCKER)
  add_custom_target(
    test-linux-docker
    COMMAND
      /usr/bin/env docker build --platform linux/${BLYT_LINUX_ARCH} -t
      blyt-testing-${BLYT_LINUX_ARCH} -f
      "${CMAKE_SOURCE_DIR}/Dockerfile.testing" "${CMAKE_SOURCE_DIR}"
    COMMAND
      /usr/bin/env docker run --platform linux/${BLYT_LINUX_ARCH} --rm -v
      "${CMAKE_SOURCE_DIR}:/repo:ro" -v
      blyt-linux-build-${BLYT_LINUX_ARCH}:/build blyt-testing-${BLYT_LINUX_ARCH}
      bash -c
      "source /opt/emsdk/emsdk_env.sh >/dev/null 2>&1; set -e && cmake -B /build -G Ninja /repo && cmake --build /build --parallel && cmake --build /build --target sdk && BLYT_BUILD_DIR=/build CARGO_HOME=/build/cargo-home CARGO_TARGET_DIR=/build/cargo-target cargo test --workspace -- ${BLYT_LINUX_TEST_FILTER} --nocapture"
    COMMENT "Building and running integration tests in Linux Docker container"
    USES_TERMINAL VERBATIM)
else()
  add_custom_target(
    test-linux-docker
    COMMAND ${CMAKE_COMMAND} -E echo
            "Docker not found — install Docker Desktop to run Linux tests"
    COMMENT "test-linux-docker requires Docker")
endif()
