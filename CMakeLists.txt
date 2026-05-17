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
add_subdirectory(third_party/flatcc EXCLUDE_FROM_ALL)

# FlatBuffers codegen — generates pure C reader headers for the runtime
# (ADR-0126)
set(SCHEMAS_DIR "${CMAKE_SOURCE_DIR}/schemas")
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
target_compile_definitions(libblytemu PRIVATE ${RV32EMU_FEATURE_DEFS})
target_compile_options(libblytemu PRIVATE -include "${RV32EMU_DIR}/common.h")
target_include_directories(libblytemu PUBLIC ${RV32EMU_DIR})
target_link_libraries(libblytemu PRIVATE libsoftfloat)

# -------------------------------------------------------------------------
# Guest-side RV32IMAFC shared libraries
#
# libblytcommon.so — variant-portable shared API (blyt.h); ECALL stubs for all
# functions declared in blyt.h.  Re-exported by each variant library.
# libblyt32.so — Blyt32 variant library; has DT_NEEDED: libblytcommon.so and
# re-exports blyt_* symbols to carts via thin wrappers.
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

  # libblyt32.so — Blyt32 variant; links against libblytcommon.so
  set(LIBBLYT32_SRC "${CMAKE_SOURCE_DIR}/runtime/guest/src/libblyt32/blyt32.c")
  set(LIBBLYT32_OUT "${CMAKE_BINARY_DIR}/libblyt32.so")
  add_custom_command(
    OUTPUT "${LIBBLYT32_OUT}"
    COMMAND
      "${CLANG_RV32}" ${RV32_FLAGS} -Wl,-soname,libblyt32.so ${RV32_INCLUDE} -L
      "${CMAKE_BINARY_DIR}" -lblytcommon -Wl,-rpath-link,"${CMAKE_BINARY_DIR}"
      -o "${LIBBLYT32_OUT}" "${LIBBLYT32_SRC}"
    DEPENDS "${LIBBLYT32_SRC}" "${BLYT_H}" "${LIBBLYTCOMMON_OUT}"
    COMMENT "Cross-compiling libblyt32.so (RV32IMAFC)"
    VERBATIM)

  add_custom_target(libblyt32_so ALL DEPENDS "${LIBBLYT32_OUT}"
                                             "${LIBBLYTCOMMON_OUT}")
  message(
    STATUS "libblytcommon.so + libblyt32.so: will be built (${CLANG_RV32})")
else()
  message(
    STATUS
      "libblytcommon.so + libblyt32.so: skipping (need clang riscv32 + ld.lld)")
endif()

# -------------------------------------------------------------------------
# Host runtime libraries (stubs for future expansion; only libblyt is real)
# -------------------------------------------------------------------------
add_library(libblytcommon INTERFACE)
add_library(libblytcommonlua INTERFACE)
add_library(libblyt32lua INTERFACE)
add_library(libblytc INTERFACE)

add_library(libblyt STATIC runtime/host/src/libblyt/cart_load.c
                           runtime/host/src/libblyt/cart_run.c)
set_target_properties(libblyt PROPERTIES OUTPUT_NAME blyt)
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
# SDL2 frontend — try find_package, then pkg-config, then sdl2-config
# -------------------------------------------------------------------------
find_package(SDL2 QUIET)
if(SDL2_FOUND OR TARGET SDL2::SDL2)
  add_executable(blytrun frontends/sdl/main.c)
  target_link_libraries(blytrun PRIVATE libblyt SDL2::SDL2)
  message(STATUS "SDL2 found via find_package — blytrun (SDL2) will be built")
else()
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(SDL2 QUIET sdl2)
  endif()
  if(SDL2_FOUND)
    add_executable(blytrun frontends/sdl/main.c)
    target_include_directories(blytrun PRIVATE ${SDL2_INCLUDE_DIRS})
    target_compile_options(blytrun PRIVATE ${SDL2_CFLAGS_OTHER})
    target_link_libraries(blytrun PRIVATE libblyt ${SDL2_LIBRARIES})
    message(STATUS "SDL2 found via pkg-config — blytrun (SDL2) will be built")
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
      add_executable(blytrun frontends/sdl/main.c)
      target_compile_options(blytrun PRIVATE ${SDL2_CFLAGS_LIST})
      target_link_options(blytrun PRIVATE ${SDL2_LDFLAGS_LIST})
      target_link_libraries(blytrun PRIVATE libblyt)
      message(
        STATUS "SDL2 found via sdl2-config — blytrun (SDL2) will be built")
    else()
      message(STATUS "SDL2 not found — blytrun (SDL2) will not be built")
    endif()
  endif()
endif()

# Frontend stub — libretro (not yet implemented)
add_library(blyt_libretro INTERFACE)

# -------------------------------------------------------------------------
# Tests
# -------------------------------------------------------------------------
enable_testing()

# Test-only FlatBuffers builder headers (runtime only generates readers)
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

# verify_cart: standalone tool used in CI to confirm blyt build output passes
# load-time security checks. Not a ctest test; invoked directly with a cart.
add_executable(verify_cart tests/tools/verify_cart.c)
target_link_libraries(verify_cart PRIVATE libblyt)

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
    -DBLYT_BINARY_DIR=${CMAKE_BINARY_DIR}
    -DCMAKE_HOST_SYSTEM_NAME=${CMAKE_HOST_SYSTEM_NAME}
    -DCMAKE_HOST_SYSTEM_PROCESSOR=${CMAKE_HOST_SYSTEM_PROCESSOR} -P
    ${CMAKE_SOURCE_DIR}/cmake/blyt_sdk.cmake
  USES_TERMINAL
  COMMENT "Assembling blyt SDK in build/sdk/"
  VERBATIM)
