cmake_minimum_required(VERSION 3.25)
project(blyt C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ccache compiler launcher (also exported to the sdk script for the nested
# libc++/WASM trees).
include(cmake/blyt_ccache.cmake)

# Version (ADR-0128): copy version.txt to build/version.txt if not already
# present (CI pre-creates it with a timestamp; local dev gets a verbatim copy).
if(NOT EXISTS "${CMAKE_BINARY_DIR}/version.txt")
  file(COPY "${CMAKE_SOURCE_DIR}/version.txt" DESTINATION "${CMAKE_BINARY_DIR}")
endif()
file(READ "${CMAKE_BINARY_DIR}/version.txt" BLYT_VERSION)
string(STRIP "${BLYT_VERSION}" BLYT_VERSION)
message(STATUS "blyt version: ${BLYT_VERSION}")

# -------------------------------------------------------------------------
# Third-party dependencies via FetchContent (issue #61)
#
# All deps are downloaded as versioned tarballs into a shared cache at
# ~/.cache/blyt/fetchcontent/ (set via FETCHCONTENT_BASE_DIR), so every worktree
# shares the same downloads.  CI caches this directory keyed on a hash of
# CMakeLists.txt.
#
# Local patch development: clone a blyt-tech fork into third_party/<dep>
# (gitignored) and check out blyt-patches-v0.  cmake auto-detects a populated
# checkout via a per-dep sentinel file and sets FETCHCONTENT_SOURCE_DIR_<dep>
# automatically — no -D flags needed. See docs/contributing/third-party.md for
# the full patch workflow.
#
# Fork tags follow v<upstream>-blyt-v0-p<N> (stable) or
# v<upstream>-blyt-v0-p<N>-<feature> (pre-release, deleted after merge). See
# scripts/release-*.sh to create tags and tarballs.
# -------------------------------------------------------------------------
include(FetchContent)

set(FETCHCONTENT_BASE_DIR
    "$ENV{HOME}/.cache/blyt/fetchcontent"
    CACHE PATH "FetchContent download cache (shared across worktrees)")

# Auto-detect local fork checkouts in third_party/ by looking for a sentinel
# file that only exists when the checkout is populated.  Empty gitlink stubs
# (e.g. uninitialised submodule directories in a new worktree) are ignored.
macro(_blyt_fc_local dep dir sentinel)
  if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/${dir}/${sentinel}")
    set("FETCHCONTENT_SOURCE_DIR_${dep}"
        "${CMAKE_SOURCE_DIR}/third_party/${dir}"
        CACHE PATH "" FORCE)
    message(STATUS "FetchContent ${dep}: using local third_party/${dir}/")
  endif()
endmacro()

_blyt_fc_local(lua lua onelua.c)
_blyt_fc_local(flatcc flatcc include/flatcc/flatcc.h)
_blyt_fc_local(rv32emu rv32emu src/riscv.h)
_blyt_fc_local(musl musl include/stdio.h)
_blyt_fc_local(libcxx libcxx runtimes/CMakeLists.txt)
_blyt_fc_local(libretro_common libretro-common include/libretro.h)
_blyt_fc_local(zstd zstd lib/zstd.h)
_blyt_fc_local(libopenmpt libopenmpt libopenmpt/libopenmpt.h)

# ── Upstream deps (auto-generated GitHub tarballs) ───────────────────────

set(FLATCC_VERSION "v0.6.1")
FetchContent_Declare(
  flatcc
  URL "https://github.com/dvidelabs/flatcc/archive/refs/tags/${FLATCC_VERSION}.tar.gz"
  URL_HASH
    SHA256=2533c2f1061498499f15acc7e0937dcf35bc68e685d237325124ae0d6c600c2b)

set(LIBRETRO_COMMON_COMMIT "ff11f01109a0a319f6f918abc706cf9a8614054e")
FetchContent_Declare(
  libretro_common
  URL "https://github.com/libretro/libretro-common/archive/${LIBRETRO_COMMON_COMMIT}.tar.gz"
  URL_HASH
    SHA256=db386da56830d294cd8ed0109c13b7cc7dad8efd29dda7847048f506951546fd)

# zstd and libopenmpt are declared but not yet used by the build; the
# declarations are here so the infrastructure is in place when needed.
set(ZSTD_VERSION "v1.5.7")
FetchContent_Declare(
  zstd
  URL "https://github.com/facebook/zstd/archive/refs/tags/${ZSTD_VERSION}.tar.gz"
  URL_HASH
    SHA256=37d7284556b20954e56e1ca85b80226768902e2edabd3b649e9e72c0c9012ee3)

set(LIBOPENMPT_VERSION "libopenmpt-0.8.7")
FetchContent_Declare(
  libopenmpt
  URL "https://github.com/OpenMPT/openmpt/archive/refs/tags/${LIBOPENMPT_VERSION}.tar.gz"
  URL_HASH
    SHA256=2a73f6aa795eced6856e480deaece29775f3d654a7fc024195a69d8d3d94f3e2)

# ── blyt-tech fork deps (custom tarballs) ────────────────────────────────
# SHA256 values below are filled in after running scripts/release-*.sh. Until
# the fork tags and tarballs exist, clone the fork into third_party/<dep> — the
# auto-detection macro above will pick it up at configure time.

# lua: blyt-tech fork adds BLYT_LUA_I32_F64 (int32 + float64) for RV32D.
set(LUA_VERSION "v5.5.0-blyt-v0-p1")
FetchContent_Declare(
  lua
  URL "https://github.com/blyt-tech/lua/archive/refs/tags/${LUA_VERSION}.tar.gz"
  URL_HASH
    SHA256=c2d9a3864e7468b4b3dc8f0548ebe82aa5dd64515f9b715342637b0f796e2feb)

# rv32emu: custom tarball with softfloat submodule embedded. Tag convention:
# g<upstream-sha>-blyt-v0-p<N> (upstream has no version tags)
set(RV32EMU_VERSION "g244cfb3-blyt-v0-p7")
FetchContent_Declare(
  rv32emu
  URL "https://github.com/blyt-tech/rv32emu/releases/download/${RV32EMU_VERSION}/rv32emu-${RV32EMU_VERSION}.tar.gz"
  URL_HASH
    SHA256=0fe03e8e8b0190a75a8561a1cfebd14f6a91a15425080d15947c8650ca2dbe9b)

# musl: auto-generated GitHub tarball from blyt-tech/musl.
set(MUSL_VERSION "v1.2.6-blyt-v0-p1")
FetchContent_Declare(
  musl
  URL "https://github.com/blyt-tech/musl/archive/refs/tags/${MUSL_VERSION}.tar.gz"
  URL_HASH
    SHA256=6ea4c369d2914a76566202438e9c52ea456c29663460d5893665298123001b67)

# libcxx: curated libcxx+libcxxabi subtree tarball from blyt-tech/llvm-project.
set(LIBCXX_VERSION "v22.1.5-blyt-v0-p4")
FetchContent_Declare(
  libcxx
  URL "https://github.com/blyt-tech/llvm-project/releases/download/${LIBCXX_VERSION}/libcxx-${LIBCXX_VERSION}.tar.gz"
  URL_HASH
    SHA256=9f74509d795211615a55f42ab414388831b4e59d43ac3e10e149de66bb4cd5b5)

# Populate deps that don't have their own CMakeLists.txt we want added as a
# cmake subdirectory.  flatcc uses FetchContent_MakeAvailable below (it has a
# CMakeLists.txt and we do want add_subdirectory). lua_POPULATED /
# rv32emu_POPULATED etc. are set by FetchContent_Populate and guard against
# double-populate across multiple include() of this file.
foreach(_dep lua rv32emu musl libcxx libretro_common zstd)
  FetchContent_GetProperties(${_dep})
  if(NOT ${_dep}_POPULATED)
    FetchContent_Populate(${_dep})
  endif()
endforeach()

# zstd decode-only static library (#157, ADR-0026): per-resource decompression
# is shared by every host-side leg (blytplay, libretro, WASM via the same
# libblyt sources). We compile only lib/common + lib/decompress — the runtime
# never compresses; the packer (devtool) owns encode. ASM is disabled so the
# pure-C decode path is bit-identical on every platform (determinism contract).
set(ZSTD_LIB_DIR "${zstd_SOURCE_DIR}/lib")
file(GLOB ZSTD_DECODE_SOURCES "${ZSTD_LIB_DIR}/common/*.c"
     "${ZSTD_LIB_DIR}/decompress/*.c")
add_library(zstd_decode STATIC ${ZSTD_DECODE_SOURCES})
set_target_properties(zstd_decode PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(zstd_decode PRIVATE ZSTD_DISABLE_ASM=1)
target_include_directories(zstd_decode PUBLIC "${ZSTD_LIB_DIR}")

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
# Newer clang versions flag [[]] empty attributes as -Wc23-extensions; flatcc
# enables -Werror for Clang, which would turn that into a build error.
set(FLATCC_ALLOW_WERROR
    OFF
    CACHE BOOL "" FORCE)
# flatcc ships cmake_minimum_required(VERSION 2.8), which CMake 4.0+ rejects.
# Populate first so we can patch the file before add_subdirectory runs.
FetchContent_GetProperties(flatcc)
if(NOT flatcc_POPULATED)
  FetchContent_Populate(flatcc)
  file(READ "${flatcc_SOURCE_DIR}/CMakeLists.txt" _flatcc_cml)
  string(REPLACE "cmake_minimum_required (VERSION 2.8)"
                 "cmake_minimum_required (VERSION 3.5)" _flatcc_cml
                 "${_flatcc_cml}")
  file(WRITE "${flatcc_SOURCE_DIR}/CMakeLists.txt" "${_flatcc_cml}")
endif()
add_subdirectory("${flatcc_SOURCE_DIR}" "${flatcc_BINARY_DIR}")
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
           "${GENERATED_DIR}/cart_layouts_reader.h"
           "${GENERATED_DIR}/cart_layouts_verifier.h"
           "${GENERATED_DIR}/flatbuffers_common_reader.h"
    COMMAND
      "$<TARGET_FILE:flatcc_cli>" --common_reader --reader --verifier -o
      "${GENERATED_DIR}" "${SCHEMAS_DIR}/cart_info.fbs"
      "${SCHEMAS_DIR}/cart_config.fbs" "${SCHEMAS_DIR}/cart_layouts.fbs"
    DEPENDS flatcc_cli "${SCHEMAS_DIR}/cart_info.fbs"
            "${SCHEMAS_DIR}/cart_config.fbs" "${SCHEMAS_DIR}/cart_layouts.fbs"
    COMMENT "Generating FlatBuffers C headers"
    VERBATIM)
  add_custom_target(
    flatbuffers_generated ALL
    DEPENDS "${GENERATED_DIR}/cart_info_reader.h"
            "${GENERATED_DIR}/cart_info_verifier.h"
            "${GENERATED_DIR}/cart_config_reader.h"
            "${GENERATED_DIR}/cart_config_verifier.h"
            "${GENERATED_DIR}/cart_layouts_reader.h"
            "${GENERATED_DIR}/cart_layouts_verifier.h"
            "${GENERATED_DIR}/flatbuffers_common_reader.h")
endif()

# -------------------------------------------------------------------------
# Berkeley SoftFloat — IEEE 754 software FP for the F extension. rv32emu's
# Makefile generates an empty platform.h; we replicate that here.
# -------------------------------------------------------------------------
set(SOFTFLOAT_SRC "${rv32emu_SOURCE_DIR}/src/softfloat/source")
set(SOFTFLOAT_PLATFORM_DIR "${CMAKE_BINARY_DIR}/softfloat")
file(MAKE_DIRECTORY "${SOFTFLOAT_PLATFORM_DIR}")
file(WRITE "${SOFTFLOAT_PLATFORM_DIR}/platform.h" "")

# rv32emu's Makefile compiles a specific subset of softfloat sources for the
# SOFTFLOAT_FAST_INT64 path.  The non-fast-int64 (*M.c helpers, BF16, and a
# handful of other files) must be excluded.  We glob the source directory then
# remove those files explicitly.
file(GLOB SOFTFLOAT_SOURCES "${SOFTFLOAT_SRC}/*.c")
# Exclude macOS AppleDouble sidecar files (._*) that libarchive may extract from
# tarballs containing LIBARCHIVE.xattr PAX headers on Linux.
list(FILTER SOFTFLOAT_SOURCES EXCLUDE REGEX "/\\._")
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
set(RV32EMU_DIR "${rv32emu_SOURCE_DIR}/src")

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
# Guest-side RV32IMAFC shared libraries → build/sdk/lib
#
# Toolchain discovery runs at configure time (cmake/blyt_rv32_toolchain.cmake);
# every guest library (release, debug, and native variants, the Lua libs, and
# blyt-luac) is a ninja rule emitting directly into the SDK layout
# (cmake/blyt_guest_libs.cmake), so a plain `cmake --build build` keeps them
# fresh — the sdk script compiles nothing RV32 itself.  The module exports
# LIBBLYTCOMMON_OUT / LIBBLYTC_OUT / LIBBLYT32_OUT / LIBBLYT32LUA_OUT (release
# paths) for the libretro embed below.
# -------------------------------------------------------------------------
include(cmake/blyt_rv32_toolchain.cmake)
include(cmake/blyt_guest_libs.cmake)

# -------------------------------------------------------------------------
# Host runtime libraries (stubs for future expansion; only libblyt is real)
# -------------------------------------------------------------------------

# Host-native Lua static library for cart inspection
# (blyt_cart_lua_lifecycle_mask). Uses onelua.c which bundles all Lua sources;
# MAKE_LIB=1 suppresses main().
set(_LUA_HOST_DIR "${lua_SOURCE_DIR}")
if(EXISTS "${_LUA_HOST_DIR}/onelua.c")
  add_library(liblua_host STATIC "${_LUA_HOST_DIR}/onelua.c")
  # MAKE_LIB=1 suppresses main(). BLYT_LUA_I32_F64=1 matches the ilp32f guest
  # bytecode integer/float sizes so the host VM can load cart bytecode compiled
  # for RISC-V. BLYT_LUA_I32_F64=1 is PUBLIC so it propagates to callers
  # (cart_load.c) that include lua.h — keeping function pointer types consistent
  # with the library.
  target_compile_definitions(
    liblua_host
    PRIVATE MAKE_LIB=1
    PUBLIC BLYT_LUA_I32_F64=1)
  target_include_directories(liblua_host PUBLIC "${_LUA_HOST_DIR}")
  target_link_libraries(liblua_host PUBLIC m)
  set_target_properties(liblua_host PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

add_library(libblytcommon INTERFACE)
add_library(libblytcommonlua INTERFACE)
add_library(libblyt32lua INTERFACE)
add_library(libblytc INTERFACE)

add_library(
  libblyt STATIC
  runtime/host/src/libblyt/cart_load.c
  runtime/host/src/libblyt/cart_run.c
  runtime/host/src/libblyt/resource.c
  runtime/host/src/libblyt/state_buffer.c
  runtime/host/src/libblyt/save.c
  runtime/host/src/libblyt/testcard.c
  runtime/host/src/libblyt/trace.c
  runtime/host/src/libblyt/version.c
  # runtime/shared: freestanding determinism-critical primitives shared with the
  # native guest libs (issue #128); defined once, compiled into both.
  runtime/shared/blyt_fp_canon.c
  runtime/shared/blyt_elf_section.c
  runtime/shared/blyt_resource_codec.c
  runtime/shared/blyt_blys.c
  runtime/shared/blyt_raster.c
  runtime/shared/blyt_frame_hash.c
  runtime/shared/blyt_palettes.c)
set_target_properties(libblyt PROPERTIES OUTPUT_NAME blyt)
# blyt_libretro.so (a shared MODULE) links libblyt.a; compile with -fPIC so the
# same static library is usable in both executables and shared objects.
set_target_properties(libblyt libblytemu libsoftfloat
                      PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(
  libblyt
  PUBLIC runtime/host/include
  PRIVATE runtime/host/src/libblyt runtime/shared ${flatcc_SOURCE_DIR}/include
          "${GENERATED_DIR}" ${RV32EMU_DIR})
# cart_run.c includes common.h explicitly; the -D flags ensure feature macros
# expand correctly when rv32emu headers are pulled in from cart_run.c.
target_compile_definitions(libblyt PRIVATE ${RV32EMU_FEATURE_DEFS})
# BLYT_VERSION carries a per-run timestamp in CI dev builds; scope it to the one
# tiny TU that uses it (blyt_runtime_version) so everything else stays
# compiler-cacheable across runs.  Directory-scoped: covers libblyt and
# libblyt_debug.
set_source_files_properties(
  runtime/host/src/libblyt/version.c PROPERTIES COMPILE_DEFINITIONS
                                                BLYT_VERSION="${BLYT_VERSION}")
target_link_libraries(
  libblyt
  PUBLIC flatccrt
  PRIVATE libblytemu zstd_decode)
if(TARGET liblua_host)
  target_link_libraries(libblyt PRIVATE liblua_host)
  target_compile_definitions(libblyt PRIVATE BLYT_HAVE_HOST_LUA=1)
endif()
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
  # ADR-0129: dap is linked into the debug runtime (libblyt_debug) only — the
  # release libblyt / blytplay / blyt_libretro carry no DAP machinery.
  message(STATUS "BLYT_DAP: DAP debugging available (debug frontend only)")
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
  # ADR-0129: gdb_stub is linked into the debug runtime (libblyt_debug) only.
  message(STATUS "BLYT_GDB: GDB RSP debugging available (debug frontend only)")
elseif(BLYT_GDB)
  message(STATUS "BLYT_GDB: disabled (Threads not found)")
else()
  message(STATUS "BLYT_GDB: GDB RSP debugging disabled")
endif()

# -------------------------------------------------------------------------
# libblyt_debug — debug variant of the host runtime (ADR-0129)
#
# Same sources as libblyt but compiled with BLYT_DAP/BLYT_GDB so cart_run.c's
# debugger hooks are present, and linked against the dap / gdb_stub transports.
# Consumed only by the blytdebug frontend; the release libblyt (and blytplay /
# blyt_libretro) stay free of all debug machinery.
# -------------------------------------------------------------------------
set(BLYT_DEBUG_RUNTIME OFF)
if((BLYT_DAP AND TARGET dap) OR (BLYT_GDB AND TARGET gdb_stub))
  set(BLYT_DEBUG_RUNTIME ON)
  add_library(
    libblyt_debug STATIC
    runtime/host/src/libblyt/cart_load.c
    runtime/host/src/libblyt/cart_run.c
    runtime/host/src/libblyt/resource.c
    runtime/host/src/libblyt/state_buffer.c
    runtime/host/src/libblyt/save.c
    runtime/host/src/libblyt/testcard.c
    runtime/host/src/libblyt/trace.c
    runtime/host/src/libblyt/version.c
    runtime/shared/blyt_fp_canon.c
    runtime/shared/blyt_elf_section.c
    runtime/shared/blyt_resource_codec.c
    runtime/shared/blyt_blys.c
    runtime/shared/blyt_raster.c
    runtime/shared/blyt_frame_hash.c
    runtime/shared/blyt_palettes.c)
  set_target_properties(libblyt_debug PROPERTIES OUTPUT_NAME blyt_debug
                                                 POSITION_INDEPENDENT_CODE ON)
  target_include_directories(
    libblyt_debug
    PUBLIC runtime/host/include
    PRIVATE runtime/host/src/libblyt runtime/shared
            ${flatcc_SOURCE_DIR}/include "${GENERATED_DIR}" ${RV32EMU_DIR})
  target_compile_definitions(libblyt_debug PRIVATE ${RV32EMU_FEATURE_DEFS})
  target_link_libraries(
    libblyt_debug
    PUBLIC flatccrt
    PRIVATE libblytemu zstd_decode)
  if(TARGET liblua_host)
    target_link_libraries(libblyt_debug PRIVATE liblua_host)
    target_compile_definitions(libblyt_debug PRIVATE BLYT_HAVE_HOST_LUA=1)
  endif()
  add_dependencies(libblyt_debug flatbuffers_generated)
  if(BLYT_DAP AND TARGET dap)
    target_compile_definitions(libblyt_debug PRIVATE BLYT_DAP=1)
    target_include_directories(libblyt_debug PRIVATE "${BLYT_DAP_DIR}")
    target_link_libraries(libblyt_debug PRIVATE dap)
  endif()
  if(BLYT_GDB AND TARGET gdb_stub)
    target_compile_definitions(libblyt_debug PRIVATE BLYT_GDB=1)
    target_include_directories(libblyt_debug PRIVATE "${BLYT_GDB_DIR}")
    target_link_libraries(libblyt_debug PRIVATE gdb_stub)
  endif()
  message(STATUS "libblyt_debug: debug runtime will be built (blytdebug)")
endif()

# -------------------------------------------------------------------------
# SDL2 frontends — try find_package, then pkg-config, then sdl2-config.
#
# Normalise whichever method succeeds into SDL2_OK plus a small set of vars,
# then build two executables (ADR-0129): blytplay  — release player, no GDB/DAP,
# loads release guest libs blytdebug — debug player, GDB/DAP on, loads debug
# guest libs (BLYT_LIB_DIR)
# -------------------------------------------------------------------------
set(SDL2_OK OFF)
set(_SDL2_TARGET "")
set(_SDL2_INCLUDE "")
set(_SDL2_LIBS "")
set(_SDL2_CFLAGS "")
set(_SDL2_LDFLAGS "")

find_package(SDL2 QUIET)
if(SDL2_FOUND OR TARGET SDL2::SDL2)
  set(SDL2_OK ON)
  set(_SDL2_TARGET SDL2::SDL2)
  message(
    STATUS "SDL2 found via find_package — blytplay/blytdebug will be built")
else()
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(SDL2 QUIET sdl2)
  endif()
  if(SDL2_FOUND)
    set(SDL2_OK ON)
    set(_SDL2_INCLUDE ${SDL2_INCLUDE_DIRS})
    set(_SDL2_LIBS ${SDL2_LIBRARIES})
    set(_SDL2_CFLAGS ${SDL2_CFLAGS_OTHER})
    message(
      STATUS "SDL2 found via pkg-config — blytplay/blytdebug will be built")
  else()
    find_program(SDL2_CONFIG sdl2-config)
    if(SDL2_CONFIG)
      execute_process(
        COMMAND ${SDL2_CONFIG} --cflags
        OUTPUT_VARIABLE _sdl2_cflags_raw
        OUTPUT_STRIP_TRAILING_WHITESPACE)
      execute_process(
        COMMAND ${SDL2_CONFIG} --libs
        OUTPUT_VARIABLE _sdl2_ldflags_raw
        OUTPUT_STRIP_TRAILING_WHITESPACE)
      separate_arguments(_SDL2_CFLAGS UNIX_COMMAND "${_sdl2_cflags_raw}")
      separate_arguments(_SDL2_LDFLAGS UNIX_COMMAND "${_sdl2_ldflags_raw}")
      set(SDL2_OK ON)
      message(
        STATUS "SDL2 found via sdl2-config — blytplay/blytdebug will be built")
    else()
      message(STATUS "SDL2 not found — blytplay/blytdebug will not be built")
    endif()
  endif()
endif()

# Define an SDL2 player executable linking the given host runtime lib.  Drives
# the runtime via the libretro API (blyt_libretro.c compiled in directly,
# without BLYT_EMBED_LIBS so dynlink falls back to BLYT_LIB_DIR).
function(blyt_add_player target runtime_lib)
  add_executable(${target} frontends/player/main.c
                           frontends/libretro/blyt_libretro.c)
  target_link_libraries(${target} PRIVATE ${runtime_lib})
  if(_SDL2_TARGET)
    target_link_libraries(${target} PRIVATE ${_SDL2_TARGET})
  endif()
  if(_SDL2_LIBS)
    target_link_libraries(${target} PRIVATE ${_SDL2_LIBS})
  endif()
  if(_SDL2_INCLUDE)
    target_include_directories(${target} PRIVATE ${_SDL2_INCLUDE})
  endif()
  if(_SDL2_CFLAGS)
    target_compile_options(${target} PRIVATE ${_SDL2_CFLAGS})
  endif()
  if(_SDL2_LDFLAGS)
    target_link_options(${target} PRIVATE ${_SDL2_LDFLAGS})
  endif()
  target_include_directories(
    ${target} PRIVATE "${libretro_common_SOURCE_DIR}/include"
                      "${CMAKE_SOURCE_DIR}/runtime/host/include")
  # Players emit directly into the SDK layout: BLYT_LIB_DIR auto-inference
  # resolves <binary>/../lib[/debug], and tests run the sdk/bin copies — so
  # building anywhere else just creates a stale duplicate.
  set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY
                                             "${CMAKE_BINARY_DIR}/sdk/bin")
endfunction()

if(SDL2_OK)
  # Release player: no debug machinery.
  blyt_add_player(blytplay libblyt)

  # Debug player: GDB/DAP on, links the debug runtime.  At runtime it loads the
  # debug guest libs via BLYT_LIB_DIR=<sdk>/lib/debug.
  if(BLYT_DEBUG_RUNTIME)
    blyt_add_player(blytdebug libblyt_debug)
    if(BLYT_DAP AND TARGET dap)
      target_compile_definitions(blytdebug PRIVATE BLYT_DAP=1)
    endif()
    if(BLYT_GDB AND TARGET gdb_stub)
      target_compile_definitions(blytdebug PRIVATE BLYT_GDB=1)
    endif()
    message(STATUS "blytdebug: debug player will be built")
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
    blyt_libretro PRIVATE "${libretro_common_SOURCE_DIR}/include"
                          "${CMAKE_SOURCE_DIR}/runtime/host/include")
  # ADR-0129: the shipped libretro core is release-only — no GDB/DAP, embeds the
  # optimized release guest libs.  Debug stepping is served via blyt debug
  # (WASM) and the native blytdebug player, not the libretro core.
  set(_blyt_libretro_defs BLYT_EMBED_LIBS)
  if(LIBBLYT32LUA_OUT)
    list(APPEND _blyt_libretro_defs BLYT_EMBED_LUA)
  endif()
  target_compile_definitions(blyt_libretro PRIVATE ${_blyt_libretro_defs})
  set_target_properties(blyt_libretro PROPERTIES PREFIX "" SUFFIX ".so")
  add_dependencies(blyt_libretro guest_libs)
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
  target_compile_definitions(blyt_native PRIVATE BLYT_WITH_CART_VALIDATE)
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
# On macOS: runs inside the blyt-testing Docker image (same image as
# test-linux-docker — gcc-riscv64-linux-gnu and ccache are baked in, and the
# blyt-compiler-cache volume is mounted), so after the image exists a launcher
# build is seconds of cache replay rather than minutes of apt install.  Docker
# must be running. On Linux (non-riscv64): runs cmake directly, relying on
# gcc-riscv64-linux-gnu being installed (e.g. sudo apt install
# gcc-riscv64-linux-gnu).
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
      # Hoisted from the test-linux-docker section below (same cache var; the
      # later set() is then a no-op): the launcher build reuses the blyt-testing
      # image and compiler-cache volume.
      set(BLYT_LINUX_ARCH
          "amd64"
          CACHE
            STRING
            "Docker target architecture for test-linux-docker (amd64 or arm64)")
      add_custom_target(
        blyt_native_riscv64
        COMMAND
          /usr/bin/env docker build --platform linux/${BLYT_LINUX_ARCH} -t
          blyt-testing-${BLYT_LINUX_ARCH} -f
          "${CMAKE_SOURCE_DIR}/Dockerfile.testing" "${CMAKE_SOURCE_DIR}"
        COMMAND
          /usr/bin/env docker run --platform linux/${BLYT_LINUX_ARCH} --rm -v
          "${CMAKE_SOURCE_DIR}:/repo" -v
          blyt-compiler-cache-${BLYT_LINUX_ARCH}:/ccache -e
          CCACHE_DIR=/ccache/ccache blyt-testing-${BLYT_LINUX_ARCH} bash -c
          [=[cd /repo && cmake -B build-riscv64 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/riscv64-linux-gnu.cmake -DBLYT_PREGENERATED_DIR=build/generated/flatbuffers && cmake --build build-riscv64 --target blyt_native && chown -R $(id -u):$(id -g) build-riscv64 2>/dev/null || true]=]
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
           "${TEST_GENERATED_DIR}/cart_layouts_builder.h"
           "${TEST_GENERATED_DIR}/flatbuffers_common_builder.h"
           "${TEST_GENERATED_DIR}/flatbuffers_common_reader.h"
           "${TEST_GENERATED_DIR}/cart_info_reader.h"
           "${TEST_GENERATED_DIR}/cart_config_reader.h"
           "${TEST_GENERATED_DIR}/cart_layouts_reader.h"
    COMMAND
      "$<TARGET_FILE:flatcc_cli>" --common --reader --builder -o
      "${TEST_GENERATED_DIR}" "${SCHEMAS_DIR}/cart_info.fbs"
      "${SCHEMAS_DIR}/cart_config.fbs" "${SCHEMAS_DIR}/cart_layouts.fbs"
    DEPENDS flatcc_cli "${SCHEMAS_DIR}/cart_info.fbs"
            "${SCHEMAS_DIR}/cart_config.fbs" "${SCHEMAS_DIR}/cart_layouts.fbs"
    COMMENT "Generating FlatBuffers test builder headers"
    VERBATIM)

  add_custom_target(
    flatbuffers_test_generated
    DEPENDS "${TEST_GENERATED_DIR}/cart_info_builder.h"
            "${TEST_GENERATED_DIR}/cart_config_builder.h"
            "${TEST_GENERATED_DIR}/cart_layouts_builder.h")
else()
  add_custom_target(flatbuffers_test_generated)
endif()

add_executable(test_cart_load tests/unit/test_cart_load.c)
target_include_directories(
  test_cart_load PRIVATE runtime/host/src/libblyt ${flatcc_SOURCE_DIR}/include
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
  add_executable(
    test_gdb_stub tests/unit/test_gdb_stub.c runtime/host/src/gdb/gdb_stub.c
                  runtime/host/src/libblyt/trace.c)
  target_include_directories(test_gdb_stub PRIVATE runtime/host/src/gdb
                                                   runtime/host/include)
  # BLYT_GDB enables stub code; BLYT_GDB_TEST exposes fc_gdb_stub_test_reset().
  # No BLYT_GDB_TCP: avoids pthreads so the test is self-contained.
  target_compile_definitions(test_gdb_stub PRIVATE BLYT_GDB=1 BLYT_GDB_TEST=1)
  add_test(NAME test_gdb_stub COMMAND test_gdb_stub)
endif()

# test_blys: pins the shared BLYS save byte format (runtime/shared/blyt_blys.c)
# in isolation — exact bytes, write→read round-trip, schema-mismatch. The module
# is freestanding, so it is compiled straight into the test executable.
add_executable(test_blys tests/unit/test_blys.c runtime/shared/blyt_blys.c)
target_include_directories(test_blys PRIVATE runtime/shared)
add_test(NAME test_blys COMMAND test_blys)

# test_handle: pins the console-wide tagged-u32 handle encoding
# (runtime/shared/blyt_handle.h, ADR-0134/ADR-0096, #195/#196) — the SURFACE /
# LOCKVIEW kinds and the reserved BLYT_SCREEN constant. Header-only and
# freestanding, so the test just needs the include dir.
add_executable(test_handle tests/unit/test_handle.c)
target_include_directories(test_handle PRIVATE runtime/shared)
add_test(NAME test_handle COMMAND test_handle)

# test_resource_lifecycle: pins the shared resource load/pin refcount state
# machine (runtime/shared/blyt_resource_lifecycle.h, #123). Header-only and
# freestanding, so the test just needs the include dir.
add_executable(test_resource_lifecycle tests/unit/test_resource_lifecycle.c)
target_include_directories(test_resource_lifecycle PRIVATE runtime/shared)
add_test(NAME test_resource_lifecycle COMMAND test_resource_lifecycle)

# test_mem_budget: pins the shared unified-memory-budget accounting primitives
# (runtime/shared/blyt_mem_budget.h, ADR-0008 #158). Header-only and
# freestanding, so the test just needs the include dir.
add_executable(test_mem_budget tests/unit/test_mem_budget.c)
target_include_directories(test_mem_budget PRIVATE runtime/shared)
add_test(NAME test_mem_budget COMMAND test_mem_budget)

# test_arena: pins the single-sourced cart-heap arena allocator
# (runtime/shared/blyt_arena.c, ADR-0008 #158) — first-fit/split/coalesce
# behaviour plus guest_heap_used accounting and the 16 MB logical cap.
add_executable(test_arena tests/unit/test_arena.c runtime/shared/blyt_arena.c)
target_include_directories(test_arena PRIVATE runtime/shared)
add_test(NAME test_arena COMMAND test_arena)

# test_palettes: pins the built-in palette resolver (runtime/shared/
# blyt_palettes.c, ADR-0042, issue #201) — handle classification (RESOURCE kind,
# RUNTIME provenance), the four built-ins each resolving to a distinct table,
# the sacrificial index 255 invariant, and aurora's pinned endpoints.
add_executable(test_palettes tests/unit/test_palettes.c
                             runtime/shared/blyt_palettes.c)
target_include_directories(test_palettes PRIVATE runtime/shared)
add_test(NAME test_palettes COMMAND test_palettes)

# test_color_constants: pins the named color-index constants shipped in the SDK
# header (runtime/guest/include/blyt.h, ADR-0059, issue #203) against the actual
# bytes of the bundled palettes — a typo'd index would still compile and draw
# *a* color, so a cart test can't catch it; this cross-checks each name's index.
# Includes the guest header host-side (declarations-only for the constants
# read).
add_executable(test_color_constants tests/unit/test_color_constants.c
                                    runtime/shared/blyt_palettes.c)
target_include_directories(test_color_constants PRIVATE runtime/guest/include
                                                        runtime/shared)
add_test(NAME test_color_constants COMMAND test_color_constants)

# test_resource_eviction: the host resource eviction mechanism + rehydration
# (runtime/host/src/libblyt/resource.c, ADR-0027 v2, #137). Links libblyt for
# the table/entry API + the shared codec; constructs entries directly via the
# test_push hook and drives evict -> rehydrate on a real zstd frame.
add_executable(test_resource_eviction tests/unit/test_resource_eviction.c)
target_include_directories(test_resource_eviction
                           PRIVATE runtime/host/src/libblyt runtime/shared)
target_link_libraries(test_resource_eviction PRIVATE libblyt)
add_test(NAME test_resource_eviction COMMAND test_resource_eviction)

# test_resource_budget: the host resource-side budget accounting +
# LRU-incremental eviction policy (runtime/host/src/libblyt/resource.c,
# ADR-0008/0027 #158) — footprint, resident-evictable, recency stamp,
# evict-to-fit (minimum, LRU-first).
add_executable(test_resource_budget tests/unit/test_resource_budget.c)
target_include_directories(test_resource_budget PRIVATE runtime/host/src/libblyt
                                                        runtime/shared)
target_link_libraries(test_resource_budget PRIVATE libblyt)
add_test(NAME test_resource_budget COMMAND test_resource_budget)

# test_resource_codec: the per-resource compression on-disk format + decode
# (runtime/shared/blyt_resource_codec.c, #157, ADR-0026). Links the decode-only
# zstd for the zstd round-trip; the encoder/threshold are devtool Rust tests.
add_executable(test_resource_codec tests/unit/test_resource_codec.c
                                   runtime/shared/blyt_resource_codec.c)
target_include_directories(test_resource_codec PRIVATE runtime/shared)
target_link_libraries(test_resource_codec PRIVATE zstd_decode)
add_test(NAME test_resource_codec COMMAND test_resource_codec)

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

# test_libretro_core: dlopens blyt_libretro.so and runs a cart through the
# libretro API (functional core test).  Built only when the libretro core is
# available (LIBBLYTC_OUT set → blyt_libretro.so will be built).
#
# ADR-0129: the libretro core is release-only — there is no debugging through
# it, so the former test_libretro_dap probe and blyt-libretro-runner
# orchestration frontend were removed (debugging is served by blytdebug / the
# debug WASM).
if(LIBBLYTC_OUT)
  add_executable(test_libretro_core tests/unit/test_libretro_core.c)
  target_include_directories(test_libretro_core
                             PRIVATE ${libretro_common_SOURCE_DIR}/include)
  target_link_libraries(test_libretro_core PRIVATE ${CMAKE_DL_LIBS})
endif()

# -------------------------------------------------------------------------
# SDK assembly target
#
# cmake --build build --target sdk
#
# The guest libraries, players, and blyt-luac are built by the main ninja graph
# directly into sdk/ (see cmake/blyt_guest_libs.cmake); the script adds the
# parts that change rarely: libc++, headers, the devtool, toolchain symlinks,
# and the WASM runtimes.  The RV32 toolchain discovered at configure time is
# passed through so the script and the ninja rules always agree.
#
# After building, carts can be built with: build/sdk/bin/blyt build
# <project-dir>
# -------------------------------------------------------------------------
add_custom_target(
  sdk
  COMMAND
    ${CMAKE_COMMAND} -DBLYT_SOURCE_DIR=${CMAKE_SOURCE_DIR}
    -DBLYT_BINARY_DIR=${CMAKE_BINARY_DIR} -DBLYT_VERSION=${BLYT_VERSION}
    -DBLYT_BUILD_NATIVE=${BLYT_BUILD_NATIVE}
    -DBLYT_RV32_CLANG=${BLYT_RV32_CLANG}
    -DBLYT_RV32_CLANGPP=${BLYT_RV32_CLANGPP} -DBLYT_RV32_LLD=${BLYT_RV32_LLD}
    -DBLYT_RV32_OBJCOPY=${BLYT_RV32_OBJCOPY} -DBLYT_RV32_AR=${BLYT_RV32_AR}
    -DBLYT_RV32_LLDB_DAP=${BLYT_RV32_LLDB_DAP}
    -DBLYT_CCACHE_PROGRAM=${BLYT_CCACHE_PROGRAM}
    -DCMAKE_HOST_SYSTEM_NAME=${CMAKE_HOST_SYSTEM_NAME}
    -DCMAKE_HOST_SYSTEM_PROCESSOR=${CMAKE_HOST_SYSTEM_PROCESSOR}
    -DBLYT_MUSL_SOURCE_DIR=${musl_SOURCE_DIR}
    -DBLYT_LIBCXX_SOURCE_DIR=${libcxx_SOURCE_DIR}
    -DBLYT_LUA_SOURCE_DIR=${lua_SOURCE_DIR}
    -DBLYT_RV32EMU_SOURCE_DIR=${rv32emu_SOURCE_DIR}
    -DBLYT_FLATCC_SOURCE_DIR=${flatcc_SOURCE_DIR}
    -DBLYT_ZSTD_SOURCE_DIR=${zstd_SOURCE_DIR}
    -DBLYT_LIBRETRO_COMMON_SOURCE_DIR=${libretro_common_SOURCE_DIR} -P
    ${CMAKE_SOURCE_DIR}/cmake/blyt_sdk.cmake
  USES_TERMINAL
  COMMENT "Assembling blyt SDK in build/sdk/"
  VERBATIM)

# The players, guest libraries, and native libs all build directly into sdk/ via
# the ninja graph.  Declare them as dependencies so `cmake --build build
# --target sdk` refreshes them before assembling the rest of the SDK.
foreach(_sdk_dep blytplay blytdebug guest_libs libblyt32_native_so
                 blyt_debug_stub)
  if(TARGET ${_sdk_dep})
    add_dependencies(sdk ${_sdk_dep})
  endif()
endforeach()

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
# Single source of truth for the format checks: CI's "Lint" step runs this
# target after configuring. Run before every commit: cmake --build build
# --target lint
#
# All first-party C sources are globbed recursively; generated cart build output
# (examples/*/build/) and vendored trees are excluded. The list is fixed at
# configure time — re-run cmake to pick up newly added files (CI always
# configures fresh, so it never sees a stale list).

file(
  GLOB_RECURSE
  _LINT_C_FILES
  "${CMAKE_SOURCE_DIR}/runtime/*.c"
  "${CMAKE_SOURCE_DIR}/runtime/*.h"
  "${CMAKE_SOURCE_DIR}/frontends/*.c"
  "${CMAKE_SOURCE_DIR}/frontends/*.h"
  "${CMAKE_SOURCE_DIR}/tests/*.c"
  "${CMAKE_SOURCE_DIR}/tests/*.h"
  "${CMAKE_SOURCE_DIR}/examples/*.c"
  "${CMAKE_SOURCE_DIR}/examples/*.h")
list(FILTER _LINT_C_FILES EXCLUDE REGEX "/(build|node_modules|target)/")

file(
  GLOB_RECURSE
  _LINT_LUA_FILES
  "${CMAKE_SOURCE_DIR}/examples/*.lua"
  "${CMAKE_SOURCE_DIR}/runtime/*.lua"
  "${CMAKE_SOURCE_DIR}/frontends/*.lua"
  "${CMAKE_SOURCE_DIR}/tests/*.lua"
  "${CMAKE_SOURCE_DIR}/sdk/*.lua"
  "${CMAKE_SOURCE_DIR}/devtool/*.lua")
list(FILTER _LINT_LUA_FILES EXCLUDE REGEX
     "/(build|node_modules|target|third_party)/")

add_custom_command(
  OUTPUT "${CMAKE_BINARY_DIR}/npm-ci.stamp"
  COMMAND npm ci
  COMMAND "${CMAKE_COMMAND}" -E touch "${CMAKE_BINARY_DIR}/npm-ci.stamp"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  DEPENDS "${CMAKE_SOURCE_DIR}/package-lock.json"
          "${CMAKE_SOURCE_DIR}/package.json"
  COMMENT "Installing JS dependencies (npm ci)"
  VERBATIM)
add_custom_target(npm_ci DEPENDS "${CMAKE_BINARY_DIR}/npm-ci.stamp")

add_custom_command(
  OUTPUT "${CMAKE_BINARY_DIR}/npm-install-wasm-tests.stamp"
  COMMAND npm install
  COMMAND npx playwright install chromium
  COMMAND "${CMAKE_COMMAND}" -E touch
          "${CMAKE_BINARY_DIR}/npm-install-wasm-tests.stamp"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/tests/wasm"
  DEPENDS "${CMAKE_SOURCE_DIR}/tests/wasm/package-lock.json"
          "${CMAKE_SOURCE_DIR}/tests/wasm/package.json"
  COMMENT "Installing WASM test dependencies (Playwright)"
  VERBATIM)
add_custom_target(wasm_test_deps
                  DEPENDS "${CMAKE_BINARY_DIR}/npm-install-wasm-tests.stamp")

# VS Code extension dependencies. tools/vscode ships its own package.json and a
# gitignored node_modules; the extension's unit tests (node --test) and a manual
# debug session both need it installed, and a fresh worktree has neither.
#
# `npm install` only runs when tools/vscode is writable. Inside
# test-linux-docker the repo is mounted read-only and npm would fail with EROFS
# writing package-lock.json — there the container reuses the host's node_modules
# (the host runs this target first, since test-linux-docker depends on it). The
# stamp itself lives under the writable build dir, so it can always be touched.
add_custom_command(
  OUTPUT "${CMAKE_BINARY_DIR}/npm-install-vscode-ext.stamp"
  COMMAND
    sh -c
    "if [ -w . ]; then npm install; else echo 'tools/vscode is read-only (container ro mount) — reusing existing node_modules'; fi"
  COMMAND "${CMAKE_COMMAND}" -E touch
          "${CMAKE_BINARY_DIR}/npm-install-vscode-ext.stamp"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/tools/vscode"
  DEPENDS "${CMAKE_SOURCE_DIR}/tools/vscode/package-lock.json"
          "${CMAKE_SOURCE_DIR}/tools/vscode/package.json"
  COMMENT "Installing VS Code extension dependencies (npm install)"
  VERBATIM)
add_custom_target(vscode_ext_deps
                  DEPENDS "${CMAKE_BINARY_DIR}/npm-install-vscode-ext.stamp")

add_custom_target(
  lint
  COMMAND clang-format --dry-run --Werror ${_LINT_C_FILES}
  COMMAND cmake-format --check "${CMAKE_SOURCE_DIR}/CMakeLists.txt"
          "${CMAKE_SOURCE_DIR}/frontends/wasm/CMakeLists.txt"
  COMMAND yamllint "${CMAKE_SOURCE_DIR}/.github/workflows/ci.yml"
  COMMAND cargo fmt --check --all
  COMMAND "${CMAKE_SOURCE_DIR}/node_modules/.bin/biome" check
          "${CMAKE_SOURCE_DIR}"
  COMMAND luacheck ${_LINT_LUA_FILES}
  COMMAND stylua --check ${_LINT_LUA_FILES}
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  COMMENT
    "Linting: clang-format, cmake-format, yamllint, cargo fmt, biome, luacheck, stylua"
  VERBATIM)
add_dependencies(lint npm_ci)

# ── fmt-fix
# ───────────────────────────────────────────────────────────────────── Fix
# counterpart to `lint`: runs all the auto-fixers in one shot so formatting
# never has to be invoked directly. Same file globs as `lint`. Run before commit
# to fix, then `lint` to verify: cmake --build build --target fmt-fix
#
# yamllint and luacheck are checkers with no auto-fix, so they have no
# counterpart here — fix their findings manually and re-run `lint`.

add_custom_target(
  fmt-fix
  COMMAND clang-format -i ${_LINT_C_FILES}
  COMMAND cmake-format -i "${CMAKE_SOURCE_DIR}/CMakeLists.txt"
          "${CMAKE_SOURCE_DIR}/frontends/wasm/CMakeLists.txt"
  COMMAND cargo fmt --all
  COMMAND "${CMAKE_SOURCE_DIR}/node_modules/.bin/biome" check --write
          "${CMAKE_SOURCE_DIR}"
  COMMAND stylua ${_LINT_LUA_FILES}
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  COMMENT "Auto-fixing: clang-format, cmake-format, cargo fmt, biome, stylua"
  VERBATIM)
add_dependencies(fmt-fix npm_ci)

# ── test-unit ─────────────────────────────────────────────────────────────────
# Run C unit tests (ctest) and Rust devtool unit tests.  Does not require the
# SDK — use after a plain `cmake --build build`.
#
# Usage: cmake --build build --target test-unit
#
add_custom_target(
  test-unit
  COMMAND ${CMAKE_CTEST_COMMAND} --test-dir ${CMAKE_BINARY_DIR}
          --output-on-failure
  COMMAND cargo nextest run -p blyt
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  USES_TERMINAL VERBATIM
  COMMENT "Running C unit tests (ctest) and Rust devtool unit tests")
# test_resource_codec is an unconditional ctest (CMakeLists.txt above) but
# builds only under the default `all` target, so a fresh worktree (or a tree set
# up via test-prereqs) fails the ctest leg with a misleading "Could not find
# executable" unless it is an explicit dependency (issue #185, same class as
# #150/#172).
add_dependencies(
  test-unit
  test_cart_load
  test_cart_run
  test_blys
  test_handle
  test_resource_lifecycle
  test_mem_budget
  test_arena
  test_palettes
  test_color_constants
  test_resource_codec
  test_resource_eviction
  test_resource_budget)
if(TARGET test_gdb_stub)
  add_dependencies(test-unit test_gdb_stub)
endif()

# ── test-integration ──────────────────────────────────────────────────────────
# Build the SDK and the RISC-V native launcher, then run the Rust integration
# suite via cargo nextest.
#
# BLYT_BUILD_DIR tells the Rust test helpers where the build tree lives.
# Integration tests always invoke the blyt binary from build/sdk/bin/ so there
# is no stale target/ binary concern.
#
# Usage: cmake --build build --target test-integration Single test: cmake
# -DBLYT_TEST_FILTER=<name> -B build cmake --build build --target
# test-integration
#
set(BLYT_TEST_FILTER
    ""
    CACHE STRING "cargo nextest filter for test-integration (e.g. lua_hello)")
if(BLYT_TEST_FILTER)
  set(_blyt_nextest_filter ${BLYT_TEST_FILTER})
else()
  set(_blyt_nextest_filter)
endif()
add_custom_target(
  test-integration
  COMMAND ${CMAKE_COMMAND} -E env BLYT_BUILD_DIR=${CMAKE_BINARY_DIR} cargo
          nextest run -p integration ${_blyt_nextest_filter}
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  USES_TERMINAL VERBATIM
  COMMENT "Running Rust integration tests (cargo nextest -p integration)")
add_dependencies(test-integration sdk)
# test_session_api exercises blyt_session_t directly from the Rust integration
# suite (require_session_api guard), but builds only under the default `all`
# target, not `sdk` — so a fresh worktree fast-fails the session/emulator tests
# at setup unless it is an explicit dependency (issue #185, same class as #172).
add_dependencies(test-integration test_session_api)
# The libretro all-legs tests dlopen blyt_libretro.so from a test_libretro_core
# harness; neither is part of `sdk` (they build under the default `all` target),
# so depend on them explicitly or a fresh worktree fails the libretro leg at
# setup (issue #172, same class as #150). Both targets only exist when the RV32
# toolchain is present (LIBBLYTC_OUT) — guard accordingly.
if(TARGET test_libretro_core)
  add_dependencies(test-integration test_libretro_core blyt_libretro)
endif()

# ── test-vscode ───────────────────────────────────────────────────────────────
# Run the VS Code extension tests against this worktree's freshly-built SDK: 1.
# extension unit tests   (node --test — cart-detection + proxy logic; cheap,
# display-free, SDK-free), then 2. the headless debugger integration suite
# (test/integration/runTests.js), which drives real blyt debug sessions through
# the extension in a headless VS Code. This is the cross-platform leg we most
# need to reproduce locally (breakpoint-verify ordering, native-vs-companion-Lua
# launch timing).
#
# The integration suite needs a display: blytdebug opens an SDL2 window (no
# --headless in native mode) and @vscode/test-electron launches a real Electron
# app. On Linux we wrap it in xvfb-run when available; on macOS it runs against
# the real display (a few windows flicker open and closed). The pinned VS Code
# build is downloaded on first run into tools/vscode/.vscode-test (override with
# BLYT_VSCODE_TEST_CACHE — the Linux Docker leg points this at a cache volume).
#
# Usage: cmake --build build --target test-vscode Narrowing (parity with
# BLYT_TEST_FILTER): cmake -DBLYT_VSCODE_GREP=<mocha name regex> -B build   #
# narrow test names cmake -DBLYT_VSCODE_CART=<cart dir|spec>    -B build   # one
# cart window cmake --build build --target test-vscode
#
set(BLYT_VSCODE_GREP
    ""
    CACHE STRING "Mocha grep for test-vscode integration suite (BLYT_IT_GREP)")
set(BLYT_VSCODE_CART
    ""
    CACHE STRING
          "Cart selector for test-vscode integration suite (BLYT_IT_CART)")
find_program(_BLYT_XVFB_RUN xvfb-run HINTS /usr/bin /usr/local/bin NO_CACHE)
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" AND _BLYT_XVFB_RUN)
  set(_blyt_vscode_it_wrap "${_BLYT_XVFB_RUN}" -a)
else()
  set(_blyt_vscode_it_wrap)
endif()
add_custom_target(
  test-vscode
  COMMAND npm test
  COMMAND
    ${_blyt_vscode_it_wrap} ${CMAKE_COMMAND} -E env
    BLYT_SDK_DIR=${CMAKE_BINARY_DIR}/sdk BLYT_IT_GREP=${BLYT_VSCODE_GREP}
    BLYT_IT_CART=${BLYT_VSCODE_CART} npm run test:integration
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/tools/vscode
  USES_TERMINAL VERBATIM
  COMMENT
    "Running VS Code extension unit tests + headless debugger integration suite"
)
add_dependencies(test-vscode sdk vscode_ext_deps)

# Fold the VS Code suite into the standard local integration flow: building
# test-integration runs test-vscode first (it is a dependency), so the one
# documented "run integration tests locally" command covers the VS Code debugger
# suite too. (Defined here, after test-vscode exists.)
add_dependencies(test-integration test-vscode)

# ── test-prereqs ──────────────────────────────────────────────────────────────
# Build/install everything the test suites need, so a fresh worktree does not
# silently skip (or fail) the wasm, native-QEMU, or VS Code legs. Aggregates the
# existing prerequisite targets in one shot:
#
# cmake-format: off
#   sdk                 players, guest libs, devtool  (most tests)
#   npm_ci              root node_modules             (biome, gdb/dap .mjs drivers)
#   wasm_test_deps      tests/wasm npm + Playwright   (browser legs)
#   vscode_ext_deps     tools/vscode npm install      (VS Code extension tests)
#   blyt_native_riscv64 build-riscv64/blyt_native     (QEMU native gate)
# cmake-format: on
#
# This target assumes the build is already configured; a brand-new worktree
# needs `cmake -B build -G Ninja` first. It does not run the tests — it only
# makes them runnable. blyt_native_riscv64 builds via Docker on macOS, so the
# blyt-testing image must exist (cheap if it does; ~15 min to build the image
# the first time).
#
# Usage: cmake --build build --target test-prereqs
#
add_custom_target(test-prereqs)
add_dependencies(test-prereqs sdk npm_ci wasm_test_deps vscode_ext_deps)
# test_session_api is needed by the Rust integration suite but builds only under
# the default `all` target, not `sdk` — without it, session-warmup leaves the
# session/emulator leg un-runnable and test-integration fast-fails (issue #185).
add_dependencies(test-prereqs test_session_api)
# blyt_native_riscv64 is a host-only convenience target that *invokes* the
# riscv64 cross build; it is not defined in the cross-compile tree itself (its
# definition is guarded by `NOT CMAKE_CROSSCOMPILING`), so only depend on it
# where it exists.
if(TARGET blyt_native_riscv64)
  add_dependencies(test-prereqs blyt_native_riscv64)
endif()
# The libretro all-legs tests need blyt_libretro.so + its test_libretro_core
# harness, which are not part of `sdk` (issue #172). Build them too so
# session-warmup leaves the libretro leg runnable. Guarded on LIBBLYTC_OUT.
if(TARGET test_libretro_core)
  add_dependencies(test-prereqs test_libretro_core blyt_libretro)
endif()

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
# Named Docker volumes used: blyt-linux-build-<arch>      build tree (cmake +
# cargo output) blyt-compiler-cache-<arch>   ccache + sccache compiler caches
# blyt-fetchcontent-cache-<arch>  FetchContent tarball downloads
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
# Opt out of the VS Code debugger integration leg inside the container (e.g.
# when iterating on a Rust-only failure). The cargo suite still runs.
option(BLYT_SKIP_VSCODE "Skip the VS Code suite in test-linux-docker" OFF)
if(BLYT_SKIP_VSCODE)
  set(_blyt_docker_vscode
      "echo '[test-linux-docker] BLYT_SKIP_VSCODE=1 — skipping VS Code suite'")
else()
  set(_blyt_docker_vscode "cmake --build /build --target test-vscode")
endif()
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
      # --shm-size: the default 64 MB /dev/shm is too small for the VS Code
      # debugger suite's Electron renderers — a webview (wasm leg) or multiple
      # debug sessions (hybrid leg) exhaust it and the renderer dies with
      # "renderer process gone (reason: crashed, code: 5)". 2 GB matches what a
      # CI runner provides.
      /usr/bin/env docker run --platform linux/${BLYT_LINUX_ARCH} --rm
      --shm-size=2g -v "${CMAKE_SOURCE_DIR}:/repo:ro" -v
      blyt-linux-build-${BLYT_LINUX_ARCH}:/build
      # Compiler caches live in their own volume so wiping the build volume (the
      # usual reset) keeps them; ccache/sccache are picked up automatically by
      # the configure step and the devtool.
      -v blyt-compiler-cache-${BLYT_LINUX_ARCH}:/ccache -e
      CCACHE_DIR=/ccache/ccache -e SCCACHE_DIR=/ccache/sccache
      # FetchContent tarball cache: persisted in a named volume so deps are not
      # re-downloaded on every docker run (mirrors the CI actions/cache).
      -v
      "blyt-fetchcontent-cache-${BLYT_LINUX_ARCH}:/root/.cache/blyt/fetchcontent"
      # VS Code build cache: the pinned @vscode/test-electron download persisted
      # in a named volume so test-vscode does not re-download VS Code every run
      # (mirrors the CI actions/cache keyed on VSCODE_VERSION). The JS modules
      # are resolved from the mounted repo's tools/vscode/node_modules
      # (installed on the host by vscode_ext_deps, a dependency of this target).
      -v "blyt-vscode-test-cache-${BLYT_LINUX_ARCH}:/vscode-test-cache" -e
      BLYT_VSCODE_TEST_CACHE=/vscode-test-cache
      # The native RISC-V QEMU gate is host-independent and would run nested
      # under Rosetta here; skip it in the Docker image (runs natively + in CI).
      -e BLYT_SKIP_QEMU_GATE=1 blyt-testing-${BLYT_LINUX_ARCH} bash -c
      "source /opt/emsdk/emsdk_env.sh >/dev/null 2>&1; set -e && cmake -B /build -G Ninja -DBLYT_BUILD_NATIVE=OFF /repo && cmake --build /build --parallel && cmake --build /build --target sdk && BLYT_BUILD_DIR=/build CARGO_HOME=/build/cargo-home CARGO_TARGET_DIR=/build/cargo-target cargo test --workspace -- ${BLYT_LINUX_TEST_FILTER} --nocapture && ${_blyt_docker_vscode}"
    COMMENT "Building and running integration tests in Linux Docker container"
    USES_TERMINAL VERBATIM)
  # node_modules is gitignored and the repo is mounted read-only, so the
  # in-container VS Code suite reuses the host's install (mirrors the Playwright
  # JS-from-host / binary-in-cache split). Ensure it exists on the host.
  add_dependencies(test-linux-docker vscode_ext_deps)
else()
  add_custom_target(
    test-linux-docker
    COMMAND ${CMAKE_COMMAND} -E echo
            "Docker not found — install Docker Desktop to run Linux tests"
    COMMENT "test-linux-docker requires Docker")
endif()
