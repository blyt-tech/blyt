# cmake/blyt_sdk.cmake
#
# Assembles a self-contained blyt SDK under build/sdk/:
#
# build/sdk/ bin/        blyt (devtool), blytplay (SDL2 runtime) include/
# blyt.h, blyt32.h, blyt_runtime.h lib/        libblytcommon.so, libblyt32.so
# (RV32IMAFC) toolchain/  clang, ld.lld, llvm-objcopy, … (downloaded LLVM)
#
# Invoked via `cmake --build build --target sdk`. Required variables (injected
# by the sdk target in CMakeLists.txt): BLYT_SOURCE_DIR, BLYT_BINARY_DIR,
# CMAKE_HOST_SYSTEM_NAME, CMAKE_HOST_SYSTEM_PROCESSOR

set(SDK_DIR "${BLYT_BINARY_DIR}/sdk")
set(SDK_BIN "${SDK_DIR}/bin")
set(SDK_LIB "${SDK_DIR}/lib")
set(SDK_INC "${SDK_DIR}/include")
set(SDK_TOOLCHAIN "${SDK_DIR}/toolchain")
set(DOWNLOADS "${BLYT_BINARY_DIR}/downloads")

# -------------------------------------------------------------------------
# Step 1: Locate or download a riscv32-capable LLVM toolchain
# -------------------------------------------------------------------------

# Check known locations for an existing riscv32-capable clang + lld pair.
set(CANDIDATE_PAIRS
    # macOS: Homebrew llvm + Homebrew lld (separate formulae on macOS)
    "/opt/homebrew/opt/llvm/bin/clang"
    "/opt/homebrew/opt/lld/bin/ld.lld"
    "/opt/homebrew/opt/llvm/bin/clang"
    "/opt/homebrew/opt/llvm/bin/ld.lld"
    "/opt/homebrew/opt/llvm/bin/clang"
    "/opt/homebrew/opt/llvm/bin/lld"
    "/usr/bin/clang-22"
    "/usr/bin/ld.lld-22"
    "/usr/bin/clang"
    "/usr/bin/ld.lld"
    "/usr/local/bin/clang"
    "/usr/local/bin/ld.lld")

set(FOUND_CLANG "")
set(FOUND_CLANGPP "")
set(FOUND_LLD "")
set(FOUND_OBJCOPY "")
set(FOUND_AR "")

list(LENGTH CANDIDATE_PAIRS PAIR_LEN)
math(EXPR PAIR_COUNT "${PAIR_LEN} / 2")
math(EXPR PAIR_LAST "${PAIR_COUNT} - 1")

foreach(IDX RANGE ${PAIR_LAST})
  math(EXPR CI "${IDX} * 2")
  math(EXPR LI "${IDX} * 2 + 1")
  list(GET CANDIDATE_PAIRS ${CI} CAND_CLANG)
  list(GET CANDIDATE_PAIRS ${LI} CAND_LLD)
  if(NOT EXISTS "${CAND_CLANG}" OR NOT EXISTS "${CAND_LLD}")
    continue()
  endif()
  execute_process(
    COMMAND "${CAND_CLANG}" --target=riscv32 -march=rv32imafc -mabi=ilp32f -x c
            -E - -o /dev/null
    INPUT_FILE /dev/null
    RESULT_VARIABLE RV32_OK
    ERROR_QUIET OUTPUT_QUIET)
  if(RV32_OK EQUAL 0)
    set(FOUND_CLANG "${CAND_CLANG}")
    set(FOUND_LLD "${CAND_LLD}")
    get_filename_component(TOOL_DIR "${CAND_CLANG}" DIRECTORY)
    get_filename_component(_CLANG_NAME "${CAND_CLANG}" NAME)
    # Prefer the versioned companion tools (e.g. clang++-22 for clang-22) so we
    # don't accidentally pick up a different version via the generic symlink.
    string(REGEX MATCH "-[0-9]+$" _VER_SUFFIX "${_CLANG_NAME}")
    if(_VER_SUFFIX AND EXISTS "${TOOL_DIR}/clang++${_VER_SUFFIX}")
      set(FOUND_CLANGPP "${TOOL_DIR}/clang++${_VER_SUFFIX}")
    elseif(EXISTS "${TOOL_DIR}/clang++")
      set(FOUND_CLANGPP "${TOOL_DIR}/clang++")
    endif()
    if(_VER_SUFFIX AND EXISTS "${TOOL_DIR}/llvm-objcopy${_VER_SUFFIX}")
      set(FOUND_OBJCOPY "${TOOL_DIR}/llvm-objcopy${_VER_SUFFIX}")
    elseif(EXISTS "${TOOL_DIR}/llvm-objcopy")
      set(FOUND_OBJCOPY "${TOOL_DIR}/llvm-objcopy")
    endif()
    if(_VER_SUFFIX AND EXISTS "${TOOL_DIR}/llvm-ar${_VER_SUFFIX}")
      set(FOUND_AR "${TOOL_DIR}/llvm-ar${_VER_SUFFIX}")
    elseif(EXISTS "${TOOL_DIR}/llvm-ar")
      set(FOUND_AR "${TOOL_DIR}/llvm-ar")
    endif()
    if(EXISTS "${TOOL_DIR}/lldb-dap")
      set(FOUND_LLDB_DAP "${TOOL_DIR}/lldb-dap")
    endif()
    break()
  endif()
endforeach()

if(NOT FOUND_CLANG)
  # Download a pre-built LLVM release.
  set(LLVM_VER "22.1.5")
  if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    if(CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL "arm64")
      set(LLVM_TRIPLE "arm64-apple-macos11")
    else()
      set(LLVM_TRIPLE "x86_64-apple-darwin")
    endif()
  elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    if(CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL "aarch64")
      set(LLVM_TRIPLE "aarch64-linux-gnu")
    else()
      set(LLVM_TRIPLE "x86_64-linux-gnu-ubuntu-24.04")
    endif()
  else()
    message(
      FATAL_ERROR "Unsupported host platform '${CMAKE_HOST_SYSTEM_NAME}'. "
                  "Set BLYT_CLANG to a clang that can target riscv32.")
  endif()

  set(LLVM_ARCHIVE "clang+llvm-${LLVM_VER}-${LLVM_TRIPLE}.tar.xz")
  set(LLVM_URL
      "https://github.com/llvm/llvm-project/releases/download/llvmorg-${LLVM_VER}/${LLVM_ARCHIVE}"
  )
  set(LLVM_ARCHIVE_PATH "${DOWNLOADS}/${LLVM_ARCHIVE}")

  if(NOT EXISTS "${SDK_TOOLCHAIN}/bin/clang")
    if(NOT EXISTS "${LLVM_ARCHIVE_PATH}")
      message(STATUS "Downloading LLVM ${LLVM_VER} (${LLVM_TRIPLE})…")
      file(MAKE_DIRECTORY "${DOWNLOADS}")
      file(
        DOWNLOAD "${LLVM_URL}" "${LLVM_ARCHIVE_PATH}"
        SHOW_PROGRESS
        STATUS DL_STATUS)
      list(GET DL_STATUS 0 DL_CODE)
      if(NOT DL_CODE EQUAL 0)
        file(REMOVE "${LLVM_ARCHIVE_PATH}")
        message(FATAL_ERROR "LLVM download failed: ${DL_STATUS}")
      endif()
    endif()

    message(STATUS "Extracting LLVM toolchain…")
    file(MAKE_DIRECTORY "${SDK_DIR}")
    execute_process(
      COMMAND ${CMAKE_COMMAND} -E tar xf "${LLVM_ARCHIVE_PATH}"
      WORKING_DIRECTORY "${SDK_DIR}"
      RESULT_VARIABLE EXTRACT_RESULT)
    if(NOT EXTRACT_RESULT EQUAL 0)
      message(FATAL_ERROR "Extraction failed")
    endif()

    file(GLOB EXTRACTED_DIR "${SDK_DIR}/clang+llvm-*")
    if(NOT EXTRACTED_DIR)
      message(
        FATAL_ERROR "Could not find extracted LLVM directory in ${SDK_DIR}")
    endif()
    file(RENAME "${EXTRACTED_DIR}" "${SDK_TOOLCHAIN}")
  endif()

  set(FOUND_CLANG "${SDK_TOOLCHAIN}/bin/clang")
  set(FOUND_CLANGPP "${SDK_TOOLCHAIN}/bin/clang++")
  set(FOUND_LLD "${SDK_TOOLCHAIN}/bin/ld.lld")
  set(FOUND_OBJCOPY "${SDK_TOOLCHAIN}/bin/llvm-objcopy")
  set(FOUND_AR "${SDK_TOOLCHAIN}/bin/llvm-ar")
  if(EXISTS "${SDK_TOOLCHAIN}/bin/lldb-dap")
    set(FOUND_LLDB_DAP "${SDK_TOOLCHAIN}/bin/lldb-dap")
  endif()
endif()

message(STATUS "blyt SDK: toolchain clang = ${FOUND_CLANG}")

# -------------------------------------------------------------------------
# Step 2: Build RV32IMAFC runtime libraries
# -------------------------------------------------------------------------

file(MAKE_DIRECTORY "${SDK_LIB}")

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
    "${BLYT_SOURCE_DIR}/runtime/guest/include")
if(FOUND_LLD)
  list(APPEND RV32_BASE "-fuse-ld=${FOUND_LLD}")
endif()

# -------------------------------------------------------------------------
# Debug/release library variants (ADR-0129)
#
# Each guest .so is built twice: release → ${SDK_LIB}        (-O2, stripped: no
# DWARF, no .symtab) debug   → ${SDK_LIB}/debug  (-O0 -g, unstripped — full
# source debugging) Determinism flags (ADR-0007) live in LIBBLYTC_CFLAGS /
# RV32_BASE and apply to both variants; only -O/-g differ.  Release dynamic libs
# are what the runtime embeds/ships; debug libs are SDK-only and loaded via
# BLYT_LIB_DIR by the debug frontends.
# -------------------------------------------------------------------------
set(SDK_LIB_DEBUG "${SDK_LIB}/debug")
file(MAKE_DIRECTORY "${SDK_LIB_DEBUG}")

# Set _VDIR (output dir), _VOPT (opt/-g flags) and _VSTRIP (strip release) for
# the named variant.  A macro (not a function) so it writes to the caller scope.
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

# Link one RV32 guest .so (ARGN = clang args after RV32_BASE) and, for release
# variants, strip DWARF + the symbol table.  --strip-unneeded keeps .dynsym so
# the library's exports (which carts link against) survive.
function(blyt_link_guest_so out_so strip)
  execute_process(COMMAND "${FOUND_CLANG}" ${RV32_BASE} ${ARGN}
                  RESULT_VARIABLE _r)
  if(NOT _r EQUAL 0)
    message(FATAL_ERROR "Failed to build ${out_so}")
  endif()
  if(strip AND FOUND_OBJCOPY)
    execute_process(COMMAND "${FOUND_OBJCOPY}" --strip-debug --strip-unneeded
                            "${out_so}" RESULT_VARIABLE _sr)
    if(NOT _sr EQUAL 0)
      message(FATAL_ERROR "Failed to strip ${out_so}")
    endif()
  endif()
endfunction()

message(STATUS "Building libblytcommon.so (release + debug)…")
foreach(_var release debug)
  blyt_set_variant(${_var})
  blyt_link_guest_so(
    "${_VDIR}/libblytcommon.so"
    ${_VSTRIP}
    ${_VOPT}
    -Wl,-soname,libblytcommon.so
    -o
    "${_VDIR}/libblytcommon.so"
    "${BLYT_SOURCE_DIR}/runtime/guest/src/libblytcommon/blyt_common.c")
endforeach()

# libblytc.so — trimmed musl-based C library (ADR-0120). Curated musl source
# subsets + our arena allocator and internal stubs.
set(MUSL_DIR "${BLYT_SOURCE_DIR}/third_party/musl")
set(LIBBLYTC_BITS_DIR "${BLYT_BINARY_DIR}/libblytc/bits")
set(LIBBLYTC_ALLTYPES_H "${LIBBLYTC_BITS_DIR}/alltypes.h")

if(NOT EXISTS "${MUSL_DIR}/include/stdio.h")
  message(FATAL_ERROR "third_party/musl not initialised. "
                      "Run: git submodule update --init third_party/musl")
endif()

# Set up bits/ directory: copy all arch/riscv32/bits/*.h files, then generate
# the two that require processing (alltypes.h from template; syscall.h from
# .in).
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
  RESULT_VARIABLE R)
if(NOT R EQUAL 0)
  message(FATAL_ERROR "Failed to generate bits/alltypes.h for libblytc.so")
endif()

# Generate bits/syscall.h: copy .in then append SYS_xxx aliases for each
# __NR_xxx (matching musl's Makefile: sed -n -e s/__NR_/SYS_/p < .in >> out).
execute_process(
  COMMAND
    sh -c "cp '${MUSL_DIR}/arch/riscv32/bits/syscall.h.in' \
        '${LIBBLYTC_BITS_DIR}/syscall.h' \
     && sed -n -e 's/__NR_/SYS_/p' \
         '${MUSL_DIR}/arch/riscv32/bits/syscall.h.in' \
         >> '${LIBBLYTC_BITS_DIR}/syscall.h'"
  RESULT_VARIABLE R)
if(NOT R EQUAL 0)
  message(FATAL_ERROR "Failed to generate bits/syscall.h for libblytc.so")
endif()

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
    "${BLYT_SOURCE_DIR}/runtime/guest/src/libblytc/blytc_arena.c"
    "${BLYT_SOURCE_DIR}/runtime/guest/src/libblytc/blytc_stubs.c")

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
  if(NOT EXISTS "${_VDIR}/libblytc.so")
    message(STATUS "Building libblytc.so (${_var})…")
    blyt_link_guest_so(
      "${_VDIR}/libblytc.so"
      ${_VSTRIP}
      ${LIBBLYTC_INCLUDES}
      ${LIBBLYTC_CFLAGS}
      ${_VOPT}
      -Wl,-soname,libblytc.so
      -o
      "${_VDIR}/libblytc.so"
      ${LIBBLYTC_SRCS})
  else()
    message(STATUS "libblytc.so (${_var}) already built — skipping")
  endif()
endforeach()

# -------------------------------------------------------------------------
# Berkeley SoftFloat for RV32 — compiler-rt soft-double/quad ABI builtins.
#
# clang emits calls to __adddf3/__muldf3/__divdf3/__udivdi3/__extendsfdf2/etc.
# for any cart C/C++/Rust code doing double or 64-bit-integer arithmetic on this
# hardware-single-float target.  musl does NOT provide these.  We compile
# SoftFloat + softfloat_builtins.c into BOTH libblyt32.so (C/C++ carts) and
# libblyt32lua.so (Lua carts) so every cart resolves them from the .so's .dynsym
# — the cart_load.c import allowlist (ADR-0112) already permits these symbols.
# Uses the same RISC-V NaN-propagation specialisation rv32emu uses on the host
# so behaviour is bit-identical to the emulator.  Defined here (not in the Lua
# block) so C/C++ support does not depend on the Lua submodule.
# -------------------------------------------------------------------------
set(SF_SRC "${BLYT_SOURCE_DIR}/third_party/rv32emu/src/softfloat/source")
if(NOT EXISTS "${SF_SRC}/f64_add.c")
  message(
    FATAL_ERROR "third_party/rv32emu SoftFloat sources not found at ${SF_SRC}. "
                "Run: git submodule update --init third_party/rv32emu")
endif()
set(SF_INC "${SF_SRC}/include")
set(SF_PLATFORM_DIR "${BLYT_BINARY_DIR}/softfloat-rv32")
file(MAKE_DIRECTORY "${SF_PLATFORM_DIR}")
# Minimal platform.h: disable thread-local so softfloat_roundingMode is global.
file(WRITE "${SF_PLATFORM_DIR}/platform.h" "#define THREAD_LOCAL\n")

# Core SoftFloat: all s_*.c and f32/f64/f128/conversion files. Exclude: extF80
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
# Shared flags/sources for any library that bakes in the soft-float builtins.
set(SF_INCLUDES -I "${SF_INC}" -I "${SF_SRC}/RISCV" -I "${SF_PLATFORM_DIR}")
set(SF_DEFINES -DSOFTFLOAT_FAST_INT64=1 -DSOFTFLOAT_ROUND_ODD=1)
set(SF_BUILTINS
    "${BLYT_SOURCE_DIR}/runtime/guest/src/libblyt32lua/softfloat_builtins.c")

message(STATUS "Building libblyt32.so…")
# libblyt32.so (emulated path) — Blyt32 ECALL stubs.
#
# Self-contained for cart LINK TIME: absorbs libblytcommon sources and ALL
# libblytc sources so carts need only -lblyt32 at build time.  Exporting
# malloc/free/string/math functions directly from libblyt32.so's .dynsym lets
# lld resolve them without adding libblytc.so to the cart's DT_NEEDED.
#
# Runtime: libblyt32.so declares DT_NEEDED: libblytc.so (forced via
# --no-as-needed).  The runtime's BFS dynamic loader picks up libblytc.so
# transitively.  The first-wins symbol rule means libblyt32.so's baked-in copies
# of malloc etc. are used on the emulated/libretro path; libblytc.so's copies
# are shadowed but the library is present for the hardware trusted-exec path
# where ld.so resolves against it directly.
foreach(_var release debug)
  blyt_set_variant(${_var})
  message(STATUS "Building libblyt32.so (${_var})…")
  blyt_link_guest_so(
    "${_VDIR}/libblyt32.so"
    ${_VSTRIP}
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
    "${BLYT_SOURCE_DIR}/runtime/guest/src/libblyt32/blyt32.c"
    "${BLYT_SOURCE_DIR}/runtime/guest/src/libblytcommon/blyt_common.c"
    ${LIBBLYTC_SRCS}
    # Soft-float / 64-bit-int compiler-rt builtins (__*df3, __udivdi3, …) so
    # C/C++ carts that do double / 64-bit math resolve them from libblyt32.so.
    ${SF_ALL}
    ${SF_RISCV}
    ${SF_MWORD}
    ${SF_BUILTINS})
endforeach()

# libblyt32.so (native path) — real API implementations for trusted native exec.
# Only used by the native RISC-V QEMU gate; gated by BLYT_BUILD_NATIVE (default
# ON, passed through from the sdk target) so environments that skip that gate
# (e.g. the Linux Docker test image) can avoid building it.
if(NOT DEFINED BLYT_BUILD_NATIVE OR BLYT_BUILD_NATIVE)
  message(STATUS "Building libblyt32.so (native build)…")
  # Compiled from frontends/native/src/libblyt32/blyt32.c.  Provides real Linux
  # syscall implementations (blyt_console_debug → write(2,...)) and the
  # restricted seccomp constructor.  Placed in sdk/lib/native/ so the launcher
  # can set LD_LIBRARY_PATH=<sdk>/lib/native to load this over the emulated one.
  set(SDK_LIB_NATIVE "${SDK_LIB}/native")
  file(MAKE_DIRECTORY "${SDK_LIB_NATIVE}")
  execute_process(
    COMMAND
      # -fno-builtin: native lib implements its own string ops and links no
      # libc, so optimisation must not rewrite them into unresolved libcalls
      # (ADR-0129).
      "${FOUND_CLANG}" ${RV32_BASE} -fno-builtin -I
      "${BLYT_SOURCE_DIR}/frontends/native/src/libblyt32"
      -Wl,-soname,libblyt32.so -Wl,--no-as-needed "${SDK_LIB}/libblytcommon.so"
      -Wl,--as-needed -o "${SDK_LIB_NATIVE}/libblyt32.so"
      "${BLYT_SOURCE_DIR}/frontends/native/src/libblyt32/blyt32.c"
    RESULT_VARIABLE R)
  # Note: libblytc.so is intentionally NOT linked here.  On native execution the
  # cart process already has musl from the ld.so interpreter; loading
  # libblytc.so (our musl subset) would create two conflicting musl instances →
  # SIGSEGV.
  if(NOT R EQUAL 0)
    message(FATAL_ERROR "Failed to build libblyt32.so (native build)")
  endif()
  # Stage libblytcommon.so into native/ so one LD_LIBRARY_PATH covers it.
  # libblytc.so is NOT staged: it must not be loaded on the native path.
  file(COPY "${SDK_LIB}/libblytcommon.so" DESTINATION "${SDK_LIB_NATIVE}")
else()
  message(STATUS "libblyt32.so (native build): skipped (BLYT_BUILD_NATIVE=OFF)")
endif()

# -------------------------------------------------------------------------
# Step 2c: Build Lua libraries for RV32IMAFC (Lua cart support)
#
# libblytcommonlua.so — Lua 5.4 VM (sandboxed: base/math/string/table/coroutine)
# libblyt32lua.so     — blyt API bindings + blyt_cart_init/update/draw lifecycle
#
# Skipped with a warning when third_party/lua is not initialised.
# -------------------------------------------------------------------------

set(LUA_DIR "${BLYT_SOURCE_DIR}/third_party/lua")

if(NOT EXISTS "${LUA_DIR}/lvm.c")
  message(
    WARNING
      "third_party/lua not initialised — Lua cart support will not be built.\n"
      "Run: git submodule update --init third_party/lua")
else()
  file(GLOB LUA_ALL_SRCS "${LUA_DIR}/*.c")
  # Remove standalone interpreter, bytecode compiler, and excluded sandboxed
  # libs.
  foreach(
    _EXCL
    "${LUA_DIR}/lua.c" # standalone interpreter binary
    "${LUA_DIR}/luac.c" # bytecode compiler binary
    "${LUA_DIR}/onelua.c" # amalgamation (includes all others — causes
                          # duplicates)
    "${LUA_DIR}/liolib.c" # I/O library (no filesystem)
    "${LUA_DIR}/loslib.c" # OS library (no OS access)
    "${LUA_DIR}/loadlib.c" # dynamic loading (no dlopen)
    "${LUA_DIR}/ldblib.c" # debug library (no debug hooks)
    "${LUA_DIR}/lutf8lib.c" # utf8 library (not needed; saves space)
  )
    list(REMOVE_ITEM LUA_ALL_SRCS "${_EXCL}")
  endforeach()

  # Public musl headers for Lua: the standard include paths minus musl's
  # internal src/include/ directory, which defines `weak` as an attribute macro
  # that collides with Lua's `GCObject *weak` field in lstate.h.
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
  # across the rv32 and WASM execution paths.  Must match every Lua build
  # (guest libraries, host WASM Lua, blyt-luac).  0x424C5954 = "BLYT".
  set(LUA_SEED_DEF "-Dluai_makeseed()=0x424C5954u")

  message(STATUS "Building libblytcommonlua.so…")
  execute_process(
    COMMAND
      "${FOUND_CLANG}" ${RV32_BASE} ${LUA_MUSL_INCLUDES} ${LIBBLYTC_CFLAGS}
      -DLUA_32BITS=1 -DLUA_USE_LONGJMP=1 ${LUA_SEED_DEF} -I "${LUA_DIR}"
      -Wl,-soname,libblytcommonlua.so -o "${SDK_LIB}/libblytcommonlua.so"
      ${LUA_ALL_SRCS}
    RESULT_VARIABLE R)
  if(NOT R EQUAL 0)
    message(FATAL_ERROR "Failed to build libblytcommonlua.so")
  endif()

  # libblyt32lua.so — blyt32 Lua bindings + symbols missing from the chain.
  #
  # libblyt32.so (always DT_NEEDED by Lua carts) already exports malloc, free,
  # memcpy, string ops, and math via its absorbed libblytc sources.
  # libblyt32lua.so adds only what libblytc does NOT provide: - setjmp / longjmp
  # (musl riscv32 asm) - soft-float ops __extendsfdf2 / __truncdfsf2 /
  # __floatdisf - errno, missing string/locale stubs, time, stdio stubs - stub
  # openers for excluded Lua standard libraries - blyt32 API Lua bindings and
  # blyt_cart_init/update/draw lifecycle
  #
  # Uses LUA_MUSL_INCLUDES (not LIBBLYTC_INCLUDES) to avoid the `#define weak
  # __attribute__((__weak__))` macro in musl/src/include/ clashing with Lua's
  # `GCObject *weak` field in lstate.h.
  #
  # SoftFloat sources/vars (SF_SRC, SF_INC, SF_ALL, SF_RISCV, SF_MWORD,
  # SF_INCLUDES, SF_DEFINES, SF_BUILTINS) are set once above the libblyt32.so
  # build and reused here.

  # libblyt32lua.so embeds the Lua VM sources directly (analogous to how
  # libblyt32.so embeds blyt_common.c), so its .dynsym exports all Lua C API
  # symbols (lua_*, luaL_*).  Carts with src/lib/ C code that call Lua APIs
  # resolve them through libblyt32lua.so; no DT_NEEDED: libblytcommonlua.so is
  # added to the cart.  libblytcommonlua.so is still built above for standalone
  # / tooling use. ADR-0129: the guest-side DAP master hook (per-instruction
  # stepping support) is debug-only.  The debug variant defines BLYT_DAP=1 and
  # links the master hook sources; the release variant drops both —
  # blyt32lua.c's hook install is `#ifdef BLYT_DAP`, so the master_hook sources
  # are unreferenced in release.
  foreach(_var release debug)
    blyt_set_variant(${_var})
    if("${_var}" STREQUAL "debug")
      set(_VLUA_DAP_FLAGS -DBLYT_DAP=1 -I
                          "${BLYT_SOURCE_DIR}/runtime/host/src/dap")
      set(_VLUA_DAP_SRCS
          "${BLYT_SOURCE_DIR}/runtime/host/src/dap/master_hook.c"
          "${BLYT_SOURCE_DIR}/runtime/guest/src/libblyt32lua/master_hook_ecall.c"
      )
    else()
      set(_VLUA_DAP_FLAGS "")
      set(_VLUA_DAP_SRCS "")
    endif()
    message(STATUS "Building libblyt32lua.so (${_var})…")
    blyt_link_guest_so(
      "${_VDIR}/libblyt32lua.so"
      ${_VSTRIP}
      ${LUA_MUSL_INCLUDES}
      ${LIBBLYTC_CFLAGS}
      ${_VOPT}
      -DLUA_32BITS=1
      -DLUA_USE_LONGJMP=1
      ${LUA_SEED_DEF}
      ${_VLUA_DAP_FLAGS}
      -I
      "${LUA_DIR}"
      -I
      "${SF_INC}"
      -I
      "${SF_SRC}/RISCV"
      -I
      "${SF_PLATFORM_DIR}"
      -DSOFTFLOAT_FAST_INT64=1
      -DSOFTFLOAT_ROUND_ODD=1
      -Wl,-soname,libblyt32lua.so
      -Wl,--version-script,${BLYT_SOURCE_DIR}/runtime/guest/src/libblyt32lua/blyt32lua.sym
      -Wl,--as-needed
      "${_VDIR}/libblyt32.so"
      -o
      "${_VDIR}/libblyt32lua.so"
      # Lua VM sources embedded directly (exports lua_*/luaL_* in .dynsym)
      ${LUA_ALL_SRCS}
      # musl riscv32 setjmp/longjmp (not in libblytc)
      "${MUSL_DIR}/src/setjmp/riscv32/setjmp.S"
      "${MUSL_DIR}/src/setjmp/riscv32/longjmp.S"
      # SoftFloat for RV32: provides f64/f128 arithmetic
      ${SF_ALL}
      ${SF_RISCV}
      ${SF_MWORD}
      # compiler-rt ABI wrappers + fenv/stdio/stdlib stubs
      "${BLYT_SOURCE_DIR}/runtime/guest/src/libblyt32lua/softfloat_builtins.c"
      "${BLYT_SOURCE_DIR}/runtime/guest/src/libblyt32lua/lua_runtime_stubs.c"
      # blyt32 API Lua bindings + cart lifecycle
      "${BLYT_SOURCE_DIR}/runtime/guest/src/libblyt32lua/blyt32lua.c"
      # DAP master hook dispatcher + ECALL stubs (debug variant only)
      ${_VLUA_DAP_SRCS})
  endforeach()

  message(STATUS "Lua libraries built: libblytcommonlua.so + libblyt32lua.so")

  # libblyt32lua.so (native path) — Lua VM + blyt bindings for trusted native exec.
  #
  # Built for the RISC-V QEMU gate.  /lib/ld-blyt.so.1 is a minimal ILP32F
  # interpreter: it does not export standard libc symbols (malloc, realloc,
  # stderr, etc.) under those names.  We embed the needed functionality directly:
  #       - libblytc_native.o: curated musl string/math/ctype/snprintf sources
  #         (compiled with LIBBLYTC_INCLUDES in a separate partial-link step to
  #         avoid the `#define weak` / Lua lstate.h collision)
  #       - lua_native_malloc.c: mmap-based malloc/free/realloc (no brk/sbrk)
  #       - lua_native_stubs.c: musl internal stubs + stdio stubs + luaopen_ stubs
  #   • Links against sdk/lib/native/libblyt32.so (real Linux syscall impls).
  #   • No BLYT_DAP flag: blyt32lua.c's DAP paths are #ifdef-guarded and compiled
  #     out; no master_hook sources are needed.
  #
  if(NOT DEFINED BLYT_BUILD_NATIVE OR BLYT_BUILD_NATIVE)
    # ── Step 1: libblytc_native.o ──────────────────────────────────────────
    # Compile a curated musl subset into a single partial-link object.
    # Compiled with LIBBLYTC_INCLUDES (includes musl/src/include/ which defines
    # `#define weak __attribute__((weak))`).  This must be a separate step from
    # the Lua VM sources, which use LUA_MUSL_INCLUDES to avoid the `weak` macro
    # colliding with Lua's `GCObject *weak` field in lstate.h.
    # Excluded from this subset:
    #   fwrite.c   — needs real musl FILE* internals; lua_native_stubs.c stubs it
    #   fmemopen.c — dead code path, complex FILE* dependencies
    #   blytc_arena.c  — requires host-side arena setup (emulated path only)
    #   blytc_stubs.c  — ECALL-based; lua_native_stubs.c replaces with native stubs
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
        "${BLYT_SOURCE_DIR}/runtime/guest/src/libblyt32lua/lua_native_fwritex.c"
        "${MUSL_DIR}/src/stdio/__overflow.c"
        "${MUSL_DIR}/src/stdio/__towrite.c"
        "${MUSL_DIR}/src/stdio/__toread.c"
        "${MUSL_DIR}/src/stdio/__uflow.c"
        "${MUSL_DIR}/src/stdio/__fmodeflags.c")
    set(_LIBBLYTC_NATIVE_OBJ "${BLYT_BINARY_DIR}/libblytc_native.o")
    # Partial-link flags: same target/arch/ABI as RV32_BASE but no -shared.
    # -no-pie prevents the clang driver injecting -pie into the lld command,
    # which would conflict with -r (relocatable partial link).
    set(_RV32_PARTIAL
        --target=riscv32-linux-gnu
        -march=rv32imafc
        -mabi=ilp32f
        -fPIC
        -nostdlib
        -no-pie)
    if(FOUND_LLD)
      list(APPEND _RV32_PARTIAL "-fuse-ld=${FOUND_LLD}")
    endif()
    execute_process(
      COMMAND "${FOUND_CLANG}" ${_RV32_PARTIAL}
              ${LIBBLYTC_INCLUDES} ${LIBBLYTC_CFLAGS} -O2
              "-Wl,-r"
              -o "${_LIBBLYTC_NATIVE_OBJ}"
              ${_LIBBLYTC_NATIVE_SRCS}
      RESULT_VARIABLE _libblytc_native_r)
    if(NOT _libblytc_native_r EQUAL 0)
      message(FATAL_ERROR "Failed to build libblytc_native.o for native libblyt32lua.so")
    endif()

    # ── Step 2: libc-stub.so (DT_NEEDED: libc.so injection) ───────────────
    # Zero-symbol stub with SONAME=libc.so.  musl's is_self path adds the ld.so
    # to the symbol chain for any reserved-name DT_NEEDED (libc., libm., …).
    # Any symbols the ld.so does export under standard names are then available.
    set(_LIBC_STUB "${BLYT_BINARY_DIR}/libc-stub.so")
    execute_process(
      COMMAND "${FOUND_CLANG}" ${RV32_BASE}
              -Wl,-soname,libc.so
              -x c /dev/null
              -o "${_LIBC_STUB}"
      RESULT_VARIABLE _libc_stub_r)
    if(NOT _libc_stub_r EQUAL 0)
      message(FATAL_ERROR "Failed to build libc-stub.so for native libblyt32lua.so")
    endif()

    # ── Step 3: link libblyt32lua.so (native) ─────────────────────────────
    message(STATUS "Building libblyt32lua.so (native build)…")
    blyt_link_guest_so(
      "${SDK_LIB}/native/libblyt32lua.so"
      TRUE
      ${LUA_MUSL_INCLUDES}
      ${LIBBLYTC_CFLAGS}
      -O2
      -DLUA_32BITS=1
      -DLUA_USE_LONGJMP=1
      ${LUA_SEED_DEF}
      -I "${LUA_DIR}"
      -I "${SF_INC}"
      -I "${SF_SRC}/RISCV"
      -I "${SF_PLATFORM_DIR}"
      -DSOFTFLOAT_FAST_INT64=1
      -DSOFTFLOAT_ROUND_ODD=1
      -Wl,-soname,libblyt32lua.so
      -Wl,--version-script,${BLYT_SOURCE_DIR}/runtime/guest/src/libblyt32lua/blyt32lua.sym
      -Wl,-z,now
      # --no-as-needed forces DT_NEEDED: libc.so even though the stub exports
      # nothing; the is_self path adds ldso to the symbol chain.
      -Wl,--no-as-needed
      "${_LIBC_STUB}"
      -Wl,--as-needed
      "${SDK_LIB}/native/libblyt32.so"
      -o
      "${SDK_LIB}/native/libblyt32lua.so"
      # Lua VM sources embedded directly (exports lua_*/luaL_* in .dynsym)
      ${LUA_ALL_SRCS}
      # musl riscv32 setjmp/longjmp
      "${MUSL_DIR}/src/setjmp/riscv32/setjmp.S"
      "${MUSL_DIR}/src/setjmp/riscv32/longjmp.S"
      # SoftFloat for RV32: f64/f128 arithmetic
      ${SF_ALL}
      ${SF_RISCV}
      ${SF_MWORD}
      # compiler-rt ABI wrappers + fenv stubs
      "${BLYT_SOURCE_DIR}/runtime/guest/src/libblyt32lua/softfloat_builtins.c"
      # curated musl string/math/ctype/snprintf (partial-link object, built above)
      "${_LIBBLYTC_NATIVE_OBJ}"
      # mmap-based malloc/free/realloc (no brk/arena — no heap conflict with ld.so)
      "${BLYT_SOURCE_DIR}/runtime/guest/src/libblyt32lua/lua_native_malloc.c"
      # musl internal stubs + stdio stubs + luaopen_ stubs
      "${BLYT_SOURCE_DIR}/runtime/guest/src/libblyt32lua/lua_native_stubs.c"
      # blyt32 API Lua bindings + cart lifecycle
      "${BLYT_SOURCE_DIR}/runtime/guest/src/libblyt32lua/blyt32lua.c")
    message(STATUS "libblyt32lua.so (native build) built")
  endif()

  file(MAKE_DIRECTORY "${SDK_BIN}")

  # Build host-native blyt-luac with -DLUA_32BITS=1.
  #
  # The RV32 and WASM Lua VMs are both compiled -DLUA_32BITS=1, so they expect
  # bytecode with 4-byte lua_Integer and 4-byte lua_Number. Using FOUND_CLANG
  # without --target=riscv32 compiles for the host. All Lua VM sources are
  # compiled in so the parser and bytecode writer are available; lua.c (has
  # main()) and onelua.c (amalgamation) are excluded.
  file(GLOB LUA_HOST_SRCS "${LUA_DIR}/*.c")
  foreach(
    _EXCL
    "${LUA_DIR}/lua.c" # standalone interpreter (defines main())
    "${LUA_DIR}/onelua.c" # amalgamation
    "${LUA_DIR}/ltests.c" # internal debug tests
  )
    list(REMOVE_ITEM LUA_HOST_SRCS "${_EXCL}")
  endforeach()

  message(STATUS "Building blyt-luac (host-native, LUA_32BITS=1)…")
  execute_process(
    COMMAND
      "${FOUND_CLANG}" -DLUA_32BITS=1 ${LUA_SEED_DEF} -O2 -I "${LUA_DIR}" -Wno-unused-parameter
      -Wno-sign-compare -Wno-implicit-fallthrough -Wno-deprecated-non-prototype
      -o "${SDK_BIN}/blyt-luac" ${LUA_HOST_SRCS}
      "${BLYT_SOURCE_DIR}/runtime/tools/blyt-luac.c" -lm
    RESULT_VARIABLE R)
  if(NOT R EQUAL 0)
    message(FATAL_ERROR "Failed to build blyt-luac")
  endif()
  message(STATUS "blyt-luac built at ${SDK_BIN}/blyt-luac")
endif()

# -------------------------------------------------------------------------
# Step 2b: Build libc++ and libc++abi for RV32IMAFC (C++ cart support)
#
# Builds a static libc++.a + libc++abi.a from third_party/libcxx (the LLVM
# monorepo fork) targeting riscv32imafc / ilp32f.  Configured with:
# -fno-exceptions -fno-rtti   (mandatory for cart C++ code)
# LIBCXX_ENABLE_THREADS=OFF   (carts are single-threaded)
# LIBCXX_ENABLE_FILESYSTEM=OFF (no filesystem access in sandboxed carts)
# LIBCXX_ENABLE_LOCALIZATION=OFF (locale state is not serialisable)
# LIBCXX_HAS_MUSL_LIBC=ON    (libblytc is our musl-derived C library)
#
# The step is skipped with a warning when: - third_party/libcxx is not
# initialised (the submodule is absent) - clang++ is not available
# (FOUND_CLANGPP is empty)
# -------------------------------------------------------------------------

set(LIBCXX_SOURCE_DIR "${BLYT_SOURCE_DIR}/third_party/libcxx")
set(LIBCXX_BUILD_DIR "${BLYT_BINARY_DIR}/build-libcxx-rv32")
set(SDK_INC_LIBCXX "${SDK_INC}/c++/v1")

if(NOT EXISTS "${LIBCXX_SOURCE_DIR}/runtimes/CMakeLists.txt")
  message(
    WARNING
      "third_party/libcxx not initialised — C++ cart support will not be built.\n"
      "Run: git submodule update --init third_party/libcxx")
elseif(NOT FOUND_CLANGPP)
  message(WARNING "clang++ not found — C++ cart support will not be built.")
elseif(EXISTS "${SDK_LIB}/libc++.a" AND EXISTS
                                        "${SDK_INC_LIBCXX}/__config_site")
  message(
    STATUS
      "libc++ already built — skipping (delete ${SDK_LIB}/libc++.a to rebuild)")
else()
  message(STATUS "Building libc++ for RV32IMAFC…")

  # Musl include paths: libcxxabi sources include <stdlib.h> via libcxx's
  # wrapper, which does #include_next <stdlib.h>.  Without these paths the cross
  # build can't find ldiv_t, lldiv_t, FP_NAN, etc.
  set(_LXX_MUSL_FLAGS
      "-isystem ${MUSL_DIR}/include -isystem ${MUSL_DIR}/arch/riscv32 -isystem ${MUSL_DIR}/arch/generic -isystem ${MUSL_DIR}/src/internal -isystem ${LIBBLYTC_BITS_DIR}/.."
  )

  # -ffunction-sections/-fdata-sections put each libc++ function/datum in its
  # own section so the cart link's --gc-sections can drop unused std-lib code
  # (e.g. the wide-char std::to_wstring/wcsto* path and aligned operator new
  # pulled in transitively by std::string but never used). Required for debug
  # cart builds, which don't use LTO. (ADR-0121 mandates LTO for release;
  # gc-sections covers both modes.)
  set(_LXX_SECTION_FLAGS "-ffunction-sections -fdata-sections")
  set(_LXX_C_FLAGS
      "--target=riscv32-linux-gnu -march=rv32imafc -mabi=ilp32f -nostdlib ${_LXX_SECTION_FLAGS} ${_LXX_MUSL_FLAGS}"
  )
  set(_LXX_CXX_FLAGS
      "--target=riscv32-linux-gnu -march=rv32imafc -mabi=ilp32f -nostdlib -fno-exceptions -fno-rtti ${_LXX_SECTION_FLAGS} ${_LXX_MUSL_FLAGS}"
  )

  if(FOUND_LLD)
    string(APPEND _LXX_C_FLAGS " -fuse-ld=${FOUND_LLD}")
    string(APPEND _LXX_CXX_FLAGS " -fuse-ld=${FOUND_LLD}")
  endif()

  # Configure
  execute_process(
    COMMAND
      ${CMAKE_COMMAND} -S "${LIBCXX_SOURCE_DIR}/runtimes" -B
      "${LIBCXX_BUILD_DIR}" -G Ninja "-DLLVM_ENABLE_RUNTIMES=libcxx;libcxxabi"
      "-DCMAKE_C_COMPILER=${FOUND_CLANG}"
      "-DCMAKE_CXX_COMPILER=${FOUND_CLANGPP}" "-DCMAKE_C_FLAGS=${_LXX_C_FLAGS}"
      "-DCMAKE_CXX_FLAGS=${_LXX_CXX_FLAGS}" -DCMAKE_BUILD_TYPE=MinSizeRel
      -DLIBCXX_ENABLE_SHARED=OFF -DLIBCXX_ENABLE_EXCEPTIONS=OFF
      -DLIBCXX_ENABLE_RTTI=OFF -DLIBCXX_ENABLE_THREADS=OFF
      -DLIBCXX_ENABLE_FILESYSTEM=OFF -DLIBCXX_ENABLE_LOCALIZATION=OFF
      -DLIBCXX_HAS_MUSL_LIBC=ON -DLIBCXX_USE_COMPILER_RT=ON
      -DLIBCXX_CXX_ABI=libcxxabi -DLIBCXX_ENABLE_STATIC_ABI_LIBRARY=ON
      -DLIBCXXABI_ENABLE_SHARED=OFF -DLIBCXXABI_ENABLE_EXCEPTIONS=OFF
      -DLIBCXXABI_ENABLE_THREADS=OFF -DLIBCXXABI_USE_COMPILER_RT=ON
      -DLIBCXXABI_USE_LLVM_UNWINDER=OFF -DLIBCXX_INCLUDE_TESTS=OFF
      -DLIBCXXABI_INCLUDE_TESTS=OFF
      # On macOS, cmake injects -arch arm64 / -isysroot into every build even
      # when cross-compiling.  Setting CMAKE_SYSTEM_NAME=Linux tells cmake this
      # is a Linux cross-compile so it suppresses all Apple toolchain defaults.
      # No-op on Linux (CMAKE_SYSTEM_NAME is already Linux).
      -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=riscv32
    RESULT_VARIABLE _LXX_CFG_R
    OUTPUT_QUIET)

  if(NOT _LXX_CFG_R EQUAL 0)
    message(FATAL_ERROR "libc++ cmake configure failed (exit ${_LXX_CFG_R})")
  endif()

  # Build — only the static libraries, not tests
  execute_process(COMMAND ${CMAKE_COMMAND} --build "${LIBCXX_BUILD_DIR}"
                          --target cxx cxxabi RESULT_VARIABLE _LXX_BUILD_R)

  if(NOT _LXX_BUILD_R EQUAL 0)
    message(FATAL_ERROR "libc++ build failed (exit ${_LXX_BUILD_R})")
  endif()

  # Copy static libraries to SDK lib/
  foreach(_lib libc++.a libc++abi.a)
    if(EXISTS "${LIBCXX_BUILD_DIR}/lib/${_lib}")
      file(COPY "${LIBCXX_BUILD_DIR}/lib/${_lib}" DESTINATION "${SDK_LIB}")
    else()
      message(
        FATAL_ERROR
          "libc++ build succeeded but ${_lib} not found in ${LIBCXX_BUILD_DIR}/lib/"
      )
    endif()
  endforeach()

  # Copy libc++ headers to SDK include/c++/v1/. Source headers come from the
  # libcxx include/ tree; cmake-generated files (__config_site,
  # __assertion_handler, etc.) come from the build tree. Copy the build tree's
  # generated include dir on top so all generated files are captured, not just
  # __config_site.
  file(MAKE_DIRECTORY "${SDK_INC_LIBCXX}")
  file(
    COPY "${LIBCXX_SOURCE_DIR}/libcxx/include/"
    DESTINATION "${SDK_INC_LIBCXX}"
    PATTERN "*.in" EXCLUDE) # exclude cmake template files
  if(EXISTS "${LIBCXX_BUILD_DIR}/include/c++/v1")
    file(COPY "${LIBCXX_BUILD_DIR}/include/c++/v1/"
         DESTINATION "${SDK_INC_LIBCXX}")
  endif()

  message(
    STATUS "libc++ built: ${SDK_LIB}/libc++.a + headers at ${SDK_INC_LIBCXX}")
endif()

# -------------------------------------------------------------------------
# Step 2b-debug: debug libc++ / libc++abi (ADR-0129)
#
# A second, unoptimised (-O0 -g) build of the static libc++ for source-level
# debugging of cart C++ code.  Same musl/cross flags as the release build (LTO
# is a separate ADR-0127 follow-up); only the archives land in ${SDK_LIB}/debug
# — headers are shared with the release build above.
# -------------------------------------------------------------------------
set(LIBCXX_DEBUG_BUILD_DIR "${BLYT_BINARY_DIR}/build-libcxx-rv32-debug")

if(NOT EXISTS "${LIBCXX_SOURCE_DIR}/runtimes/CMakeLists.txt" OR NOT
                                                                FOUND_CLANGPP)
  # Skipped: same conditions as the release build warn above.
elseif(EXISTS "${SDK_LIB_DEBUG}/libc++.a")
  message(
    STATUS
      "debug libc++ already built — skipping (delete ${SDK_LIB_DEBUG}/libc++.a to rebuild)"
  )
else()
  message(STATUS "Building debug libc++ for RV32IMAFC (-O0 -g)…")

  set(_LXXD_MUSL_FLAGS
      "-isystem ${MUSL_DIR}/include -isystem ${MUSL_DIR}/arch/riscv32 -isystem ${MUSL_DIR}/arch/generic -isystem ${MUSL_DIR}/src/internal -isystem ${LIBBLYTC_BITS_DIR}/.."
  )
  set(_LXXD_SECTION_FLAGS "-ffunction-sections -fdata-sections -O0 -g")
  set(_LXXD_C_FLAGS
      "--target=riscv32-linux-gnu -march=rv32imafc -mabi=ilp32f -nostdlib ${_LXXD_SECTION_FLAGS} ${_LXXD_MUSL_FLAGS}"
  )
  set(_LXXD_CXX_FLAGS
      "--target=riscv32-linux-gnu -march=rv32imafc -mabi=ilp32f -nostdlib -fno-exceptions -fno-rtti ${_LXXD_SECTION_FLAGS} ${_LXXD_MUSL_FLAGS}"
  )
  if(FOUND_LLD)
    string(APPEND _LXXD_C_FLAGS " -fuse-ld=${FOUND_LLD}")
    string(APPEND _LXXD_CXX_FLAGS " -fuse-ld=${FOUND_LLD}")
  endif()

  execute_process(
    COMMAND
      ${CMAKE_COMMAND} -S "${LIBCXX_SOURCE_DIR}/runtimes" -B
      "${LIBCXX_DEBUG_BUILD_DIR}" -G Ninja
      "-DLLVM_ENABLE_RUNTIMES=libcxx;libcxxabi"
      "-DCMAKE_C_COMPILER=${FOUND_CLANG}"
      "-DCMAKE_CXX_COMPILER=${FOUND_CLANGPP}" "-DCMAKE_C_FLAGS=${_LXXD_C_FLAGS}"
      "-DCMAKE_CXX_FLAGS=${_LXXD_CXX_FLAGS}" -DCMAKE_BUILD_TYPE=Debug
      -DLIBCXX_ENABLE_SHARED=OFF -DLIBCXX_ENABLE_EXCEPTIONS=OFF
      -DLIBCXX_ENABLE_RTTI=OFF -DLIBCXX_ENABLE_THREADS=OFF
      -DLIBCXX_ENABLE_FILESYSTEM=OFF -DLIBCXX_ENABLE_LOCALIZATION=OFF
      -DLIBCXX_HAS_MUSL_LIBC=ON -DLIBCXX_USE_COMPILER_RT=ON
      -DLIBCXX_CXX_ABI=libcxxabi -DLIBCXX_ENABLE_STATIC_ABI_LIBRARY=ON
      -DLIBCXXABI_ENABLE_SHARED=OFF -DLIBCXXABI_ENABLE_EXCEPTIONS=OFF
      -DLIBCXXABI_ENABLE_THREADS=OFF -DLIBCXXABI_USE_COMPILER_RT=ON
      -DLIBCXXABI_USE_LLVM_UNWINDER=OFF -DLIBCXX_INCLUDE_TESTS=OFF
      -DLIBCXXABI_INCLUDE_TESTS=OFF -DCMAKE_SYSTEM_NAME=Linux
      -DCMAKE_SYSTEM_PROCESSOR=riscv32
    RESULT_VARIABLE _LXXD_CFG_R
    OUTPUT_QUIET)
  if(NOT _LXXD_CFG_R EQUAL 0)
    message(
      FATAL_ERROR "debug libc++ cmake configure failed (exit ${_LXXD_CFG_R})")
  endif()

  execute_process(COMMAND ${CMAKE_COMMAND} --build "${LIBCXX_DEBUG_BUILD_DIR}"
                          --target cxx cxxabi RESULT_VARIABLE _LXXD_BUILD_R)
  if(NOT _LXXD_BUILD_R EQUAL 0)
    message(FATAL_ERROR "debug libc++ build failed (exit ${_LXXD_BUILD_R})")
  endif()

  foreach(_lib libc++.a libc++abi.a)
    if(EXISTS "${LIBCXX_DEBUG_BUILD_DIR}/lib/${_lib}")
      file(COPY "${LIBCXX_DEBUG_BUILD_DIR}/lib/${_lib}"
           DESTINATION "${SDK_LIB_DEBUG}")
    else()
      message(
        FATAL_ERROR
          "debug libc++ build succeeded but ${_lib} not found in ${LIBCXX_DEBUG_BUILD_DIR}/lib/"
      )
    endif()
  endforeach()

  message(STATUS "debug libc++ built: ${SDK_LIB_DEBUG}/libc++.a")
endif()

# -------------------------------------------------------------------------
# Step 3: SDK headers
# -------------------------------------------------------------------------

# Start with a clean include/ so removed headers don't linger across rebuilds.
file(REMOVE_RECURSE "${SDK_INC}")
file(MAKE_DIRECTORY "${SDK_INC}")
file(
  COPY "${BLYT_SOURCE_DIR}/runtime/guest/include/blyt.h"
       "${BLYT_SOURCE_DIR}/runtime/guest/include/blyt32.h"
       "${BLYT_SOURCE_DIR}/runtime/guest/include/blyt_lua_internal.h"
  DESTINATION "${SDK_INC}")

# Copy the curated subset of musl public headers that correspond to functions
# libblytc.so actually provides (ADR-0120).  Excluded: filesystem I/O (fopen,
# unistd, dirent), pthreads, signals, dynamic loading, networking. Cart code
# that includes an omitted header gets a compile-time error rather than a
# confusing link-time undefined-symbol failure.
foreach(
  _H
  # Infrastructure pulled in by most musl headers
  features.h
  alloca.h
  stdc-predef.h
  # Our curated API surface (ADR-0120)
  assert.h
  complex.h
  ctype.h
  endian.h
  errno.h
  fenv.h
  float.h
  inttypes.h
  iso646.h
  limits.h
  locale.h
  math.h
  stdarg.h
  stdalign.h
  stdbool.h
  stddef.h
  stdint.h
  stdnoreturn.h
  stdio.h
  stdlib.h
  string.h
  strings.h
  time.h
  uchar.h
  wchar.h
  wctype.h)
  file(COPY "${MUSL_DIR}/include/${_H}" DESTINATION "${SDK_INC}")
endforeach()

# subdirectory headers needed by the above
file(COPY "${MUSL_DIR}/include/sys/types.h" DESTINATION "${SDK_INC}/sys")

# bits/ (generated): alltypes.h, syscall.h, and arch-specific headers. Also copy
# arch/generic/bits/ which has stdint.h (fast integer types) included
# unconditionally by musl's <stdint.h>.
file(COPY "${MUSL_DIR}/arch/generic/bits/" DESTINATION "${SDK_INC}/bits")
file(COPY "${LIBBLYTC_BITS_DIR}/" DESTINATION "${SDK_INC}/bits")

# Restore libc++ headers after the REMOVE_RECURSE above wiped SDK_INC. Step 2b
# installs them into SDK_INC/c++/v1/ but Step 3 cleans the whole directory;
# re-copy them here so cart C++ builds find their headers.
if(EXISTS "${SDK_LIB}/libc++.a")
  set(SDK_INC_LIBCXX "${SDK_INC}/c++/v1")
  file(MAKE_DIRECTORY "${SDK_INC_LIBCXX}")
  file(
    COPY "${LIBCXX_SOURCE_DIR}/libcxx/include/"
    DESTINATION "${SDK_INC_LIBCXX}"
    PATTERN "*.in" EXCLUDE)
  if(EXISTS "${LIBCXX_BUILD_DIR}/include/c++/v1")
    file(COPY "${LIBCXX_BUILD_DIR}/include/c++/v1/"
         DESTINATION "${SDK_INC_LIBCXX}")
  endif()
endif()

# Lua headers (lua.h, lauxlib.h, lualib.h, luaconf.h) are NOT installed in the
# public SDK include path.  Cart code must not include them directly; use
# BLYT_LUA_EXPORT_* macros which pull in blyt_lua_internal.h automatically. The
# raw Lua C API headers are SDK-internal and only used by devtool-generated
# cart_lua_modules glue code that the cart developer never writes by hand.

# -------------------------------------------------------------------------
# Step 4: blyt devtool
# -------------------------------------------------------------------------

file(MAKE_DIRECTORY "${SDK_BIN}")
set(_cargo_target_dir "${BLYT_BINARY_DIR}/cargo-target")
set(ENV{CARGO_HOME} "${BLYT_BINARY_DIR}/cargo-home")
set(ENV{CARGO_TARGET_DIR} "${_cargo_target_dir}")
execute_process(
  COMMAND cargo build --manifest-path "${BLYT_SOURCE_DIR}/Cargo.toml" --bin blyt
  RESULT_VARIABLE R)
unset(ENV{CARGO_HOME})
unset(ENV{CARGO_TARGET_DIR})
if(NOT R EQUAL 0)
  message(FATAL_ERROR "Failed to build blyt devtool")
endif()
file(COPY "${_cargo_target_dir}/debug/blyt" DESTINATION "${SDK_BIN}")

# Copy Rust SDK crate so blyt can find it via sdk_root_from_exe().
file(COPY "${BLYT_SOURCE_DIR}/sdk/rust" DESTINATION "${SDK_DIR}")

# Expose toolchain binaries under blyt-prefixed names in bin/.
#
# blyt build uses -fuse-ld=<absolute-path-to-blyt-ld.lld> when the SDK lld is
# available, so the exact binary validated at SDK assembly time is always used —
# no dependency on PATH ordering or clang's own-directory lookup. The blyt-
# prefix keeps the name distinct from system ld.lld when sdk/bin/ is on PATH or
# installed into a shared directory like /usr/bin.  FOUND_* may point into a
# downloaded SDK_TOOLCHAIN (macOS) or to system tools (Linux); we create the
# symlinks in both cases.
#
# Remove any stale symlinks from prior builds so no dead entries linger.
foreach(
  _stale
  blyt-clang
  blyt-clang++
  blyt-ld.lld
  blyt-lld
  blyt-objcopy
  blyt-llvm-ar
  blyt-lldb-dap
  ld.lld
  llvm-objcopy)
  file(REMOVE "${SDK_BIN}/${_stale}")
endforeach()
if(FOUND_CLANG)
  file(CREATE_LINK "${FOUND_CLANG}" "${SDK_BIN}/blyt-clang" SYMBOLIC)
endif()
if(FOUND_CLANGPP)
  file(CREATE_LINK "${FOUND_CLANGPP}" "${SDK_BIN}/blyt-clang++" SYMBOLIC)
endif()
if(FOUND_LLD)
  # blyt-ld.lld: the SDK's private lld, referenced via absolute path in blyt
  # build (-fuse-ld=<abs-path>).  The blyt- prefix avoids any clash with system
  # ld.lld when sdk/bin/ is on PATH or installed into /usr/bin.
  file(CREATE_LINK "${FOUND_LLD}" "${SDK_BIN}/blyt-ld.lld" SYMBOLIC)
endif()
if(FOUND_OBJCOPY)
  file(CREATE_LINK "${FOUND_OBJCOPY}" "${SDK_BIN}/blyt-objcopy" SYMBOLIC)
endif()
if(FOUND_AR)
  file(CREATE_LINK "${FOUND_AR}" "${SDK_BIN}/blyt-llvm-ar" SYMBOLIC)
endif()
if(FOUND_LLDB_DAP)
  file(CREATE_LINK "${FOUND_LLDB_DAP}" "${SDK_BIN}/blyt-lldb-dap" SYMBOLIC)
endif()
# -------------------------------------------------------------------------
# Step 5: blytplay (if built)
# -------------------------------------------------------------------------

if(EXISTS "${BLYT_BINARY_DIR}/blytplay")
  file(COPY "${BLYT_BINARY_DIR}/blytplay" DESTINATION "${SDK_BIN}")
endif()
# ADR-0129: ship the debug player (blytdebug) in the SDK too.  It is SDK-only —
# never embedded in / shipped with the production runtime.
if(EXISTS "${BLYT_BINARY_DIR}/blytdebug")
  file(COPY "${BLYT_BINARY_DIR}/blytdebug" DESTINATION "${SDK_BIN}")
endif()

# -------------------------------------------------------------------------
# Step 6: blyt_libretro.so — host-side libretro core
#
# The libretro core embeds the guest .so files built above so it is
# self-contained.  It links against the cmake-built host static libs (libblyt,
# rv32emu, softfloat, flatccrt).
#
# If the cmake main build already produced blyt_libretro.so (Linux CI, where lld
# is available), that file is used as-is.  Otherwise the core is compiled here
# using the host C compiler.
# -------------------------------------------------------------------------

set(LIBRETRO_OUT "${BLYT_BINARY_DIR}/blyt_libretro.so")

if(EXISTS "${LIBRETRO_OUT}")
  message(STATUS "blyt_libretro.so: already built by cmake — using existing")
else()
  message(STATUS "Building blyt_libretro.so (host)…")

  set(EMBEDDED_LIBS_C "${BLYT_BINARY_DIR}/blyt_embedded_libs.c")

  if(EXISTS "${SDK_LIB}/libblyt32lua.so")
    execute_process(
      COMMAND
        python3 "${BLYT_SOURCE_DIR}/cmake/bin2c.py" blytcommon_so
        "${SDK_LIB}/libblytcommon.so" blytc_so "${SDK_LIB}/libblytc.so"
        blyt32_so "${SDK_LIB}/libblyt32.so" blyt32lua_so
        "${SDK_LIB}/libblyt32lua.so" "${EMBEDDED_LIBS_C}"
      RESULT_VARIABLE R)
    set(_libretro_embed_defs "-DBLYT_EMBED_LIBS" "-DBLYT_EMBED_LUA")
  else()
    execute_process(
      COMMAND
        python3 "${BLYT_SOURCE_DIR}/cmake/bin2c.py" blytcommon_so
        "${SDK_LIB}/libblytcommon.so" blytc_so "${SDK_LIB}/libblytc.so"
        blyt32_so "${SDK_LIB}/libblyt32.so" "${EMBEDDED_LIBS_C}"
      RESULT_VARIABLE R)
    set(_libretro_embed_defs "-DBLYT_EMBED_LIBS")
  endif()
  if(NOT R EQUAL 0)
    message(FATAL_ERROR "Failed to generate blyt_embedded_libs.c")
  endif()

  # Locate host C compiler (prefer cc, fall back to clang/gcc)
  find_program(HOST_CC NAMES cc clang gcc)
  if(NOT HOST_CC)
    message(FATAL_ERROR "Could not find a host C compiler (cc/clang/gcc)")
  endif()

  # Platform-specific shared-library flags
  if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    set(LINK_FLAGS -bundle -undefined dynamic_lookup)
  else()
    set(LINK_FLAGS -shared)
  endif()

  execute_process(
    COMMAND
      "${HOST_CC}" ${LINK_FLAGS} -fPIC -o "${LIBRETRO_OUT}"
      "-DBLYT_VERSION=\"${BLYT_VERSION}\"" ${_libretro_embed_defs}
      "${BLYT_SOURCE_DIR}/frontends/libretro/blyt_libretro.c"
      "${EMBEDDED_LIBS_C}" -I
      "${BLYT_SOURCE_DIR}/third_party/libretro-common/include" -I
      "${BLYT_SOURCE_DIR}/runtime/host/include" "${BLYT_BINARY_DIR}/libblyt.a"
      "${BLYT_BINARY_DIR}/liblibblytemu.a"
      "${BLYT_BINARY_DIR}/liblibsoftfloat.a"
      "${BLYT_SOURCE_DIR}/third_party/flatcc/lib/libflatccrt.a"
    RESULT_VARIABLE R)
  if(NOT R EQUAL 0)
    message(FATAL_ERROR "Failed to build blyt_libretro.so")
  endif()

  # macOS requires a code signature on dynamically loaded bundles; use ad-hoc
  # signing (no developer certificate) so dlopen succeeds under SIP.
  if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    find_program(CODESIGN codesign)
    if(CODESIGN)
      execute_process(COMMAND "${CODESIGN}" --force --sign - "${LIBRETRO_OUT}"
                      RESULT_VARIABLE R)
      if(NOT R EQUAL 0)
        message(FATAL_ERROR "codesign failed for blyt_libretro.so")
      endif()
    endif()
  endif()

  message(STATUS "Built ${LIBRETRO_OUT}")
endif()

# -------------------------------------------------------------------------
# Step 7: WASM runtime (optional — requires Emscripten)
#
# Builds blytplay.js + blytplay.wasm using the guest libraries from Step 2.
# Outputs land in sdk/share/wasm/ so `blyt run` can locate them and developers
# can embed them directly without any build commands.
#
# Skipped silently when emcc is not on PATH.
# -------------------------------------------------------------------------

set(SDK_SHARE_WASM "${SDK_DIR}/share/wasm")
set(SDK_SHARE_WASM_DEBUG "${SDK_DIR}/share/wasm-debug")

find_program(EMCC emcc)
if(EMCC)
  message(STATUS "Building WASM runtimes (emcc found at ${EMCC})…")
  file(MAKE_DIRECTORY "${SDK_SHARE_WASM}")
  file(MAKE_DIRECTORY "${SDK_SHARE_WASM_DEBUG}")

  # ADR-0129: build the WASM player twice. release → blytplay.*  (DAP/GDB OFF,
  # release libs) → share/wasm debug   → blytdebug.* (DAP/GDB ON,  debug libs) →
  # share/wasm-debug `blyt run` serves the release set; `blyt debug` serves the
  # debug set.
  foreach(_w release debug)
    if("${_w}" STREQUAL "debug")
      set(_WDIR "${BLYT_BINARY_DIR}/build-wasm-debug")
      set(_WLIBS "${SDK_LIB_DEBUG}")
      set(_WNAME "blytdebug")
      set(_WDEST "${SDK_SHARE_WASM_DEBUG}")
      set(_WDBG ON)
    else()
      set(_WDIR "${BLYT_BINARY_DIR}/build-wasm")
      set(_WLIBS "${SDK_LIB}")
      set(_WNAME "blytplay")
      set(_WDEST "${SDK_SHARE_WASM}")
      set(_WDBG OFF)
    endif()

    message(STATUS "Building ${_WNAME} WASM runtime (${_w})…")
    execute_process(
      COMMAND
        emcmake ${CMAKE_COMMAND} -B "${_WDIR}" -S
        "${BLYT_SOURCE_DIR}/frontends/wasm" "-DBLYT_GUEST_LIB_DIR=${_WLIBS}"
        "-DBLYT_VERSION=${BLYT_VERSION}" "-DBLYT_WASM_NAME=${_WNAME}"
        "-DBLYT_DAP=${_WDBG}" "-DBLYT_GDB=${_WDBG}" -G Ninja
      RESULT_VARIABLE R
      OUTPUT_QUIET)
    if(NOT R EQUAL 0)
      message(WARNING "${_WNAME} WASM: configure failed — skipping")
    else()
      execute_process(COMMAND ${CMAKE_COMMAND} --build "${_WDIR}"
                      RESULT_VARIABLE R)
      if(NOT R EQUAL 0)
        message(WARNING "${_WNAME} WASM: build failed — skipping")
      else()
        foreach(_ext js wasm html)
          if(EXISTS "${_WDIR}/${_WNAME}.${_ext}")
            file(COPY "${_WDIR}/${_WNAME}.${_ext}" DESTINATION "${_WDEST}")
          endif()
        endforeach()
        message(STATUS "${_WNAME} WASM assembled at ${_WDEST}")
      endif()
    endif()
  endforeach()

  # Write the embedding README alongside the release runtime files.
  file(
    WRITE "${SDK_SHARE_WASM}/README.md"
    "# blytplay WASM runtime\n\
\n\
`blytplay.js` and `blytplay.wasm` are the blyt emulator compiled to WebAssembly\n\
via Emscripten.  Drop them on any HTTP server alongside your cart and a page\n\
that wires up the canvas — no build tools required.\n\
\n\
## Requirements\n\
\n\
- Files must be served over HTTP (not `file://`) — browsers block WASM loading\n\
  from the local filesystem.\n\
- The server must send `Content-Type: application/wasm` for `.wasm` files;\n\
  most web servers do this automatically.\n\
- A `<canvas id=\"canvas\" width=\"320\" height=\"240\">` element must exist\n\
  before the script runs.\n\
\n\
## Minimal page\n\
\n\
```html\n\
<!doctype html>\n\
<html>\n\
<head><meta charset=\"utf-8\"><title>My Game</title></head>\n\
<body style=\"background:#111;display:flex;justify-content:center\">\n\
  <canvas id=\"canvas\" width=\"320\" height=\"240\"\n\
          style=\"width:640px;height:480px;image-rendering:pixelated\"></canvas>\n\
  <script>\n\
    var Module = {\n\
      canvas: document.getElementById(\"canvas\"),\n\
      preRun: [function() {\n\
        /* Fetch the cart and write it into the WASM virtual filesystem\n\
         * before main() starts.  addRunDependency delays startup until\n\
         * the fetch completes. */\n\
        Module.addRunDependency(\"cart\");\n\
        fetch(\"my-game.blyt\")\n\
          .then(function(r) { return r.arrayBuffer(); })\n\
          .then(function(buf) {\n\
            FS.writeFile(\"/cart.blyt\", new Uint8Array(buf));\n\
            Module.removeRunDependency(\"cart\");\n\
          });\n\
      }],\n\
      print:    function(s) { console.log(s); },\n\
      printErr: function(s) { console.error(s); },\n\
    };\n\
  </script>\n\
  <script src=\"blytplay.js\"></script>\n\
</body>\n\
</html>\n\
```\n\
\n\
Replace `my-game.blyt` with the path to your cart relative to the page.\n\
\n\
## Development server\n\
\n\
`blyt run <cart.blyt>` serves the runtime and cart together on a local port\n\
and opens the correct URL.  It uses an internal shell page; for a\n\
production-ready page use the template above as a starting point.\n\
")

  message(STATUS "WASM runtimes assembled:")
  message(STATUS "  release: ${SDK_SHARE_WASM} (blyt run)")
  message(STATUS "  debug:   ${SDK_SHARE_WASM_DEBUG} (blyt debug)")
  message(STATUS "  Embed in a page: see ${SDK_SHARE_WASM}/README.md")
else()
  message(
    STATUS
      "blytplay WASM: skipped (emcc not found — install Emscripten to build WASM runtime)"
  )
endif()

# -------------------------------------------------------------------------
# Summary
# -------------------------------------------------------------------------

message(STATUS "blyt SDK assembled at ${SDK_DIR}")
message(STATUS "  bin:       ${SDK_BIN}")
message(STATUS "  include:   ${SDK_INC}")
message(STATUS "  lib:       ${SDK_LIB}")
message(STATUS "  share:     ${SDK_DIR}/share/")
if(EXISTS "${SDK_TOOLCHAIN}")
  message(STATUS "  toolchain: ${SDK_TOOLCHAIN}")
endif()
message(STATUS "")
message(
  STATUS
    "Build a cart:  BLYT_SDK_DIR=${SDK_DIR} ${SDK_BIN}/blyt build <project>")
