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
# rv32emu — user-mode RISC-V emulator, built from source as a static library. We
# compile only the user-mode sources (no JIT, no SDL, no system/MMU, no GDBSTUB)
# and disable EXT_F to avoid the uninitialized softfloat submodule (Phase 3
# carts do not use floating-point instructions).
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
    RV32_FEATURE_ARCH_TEST=0
    RV32_FEATURE_EXT_F=0)

# For libblytemu: force-include common.h so that every rv32emu .c file gets
# RV32_HAS, FORCE_INLINE, HAVE_MMAP, etc. without needing to include it
# explicitly (rv32emu's own Makefile uses a generated config.h for this).
target_compile_definitions(libblytemu PRIVATE ${RV32EMU_FEATURE_DEFS})
target_compile_options(libblytemu PRIVATE -include "${RV32EMU_DIR}/common.h")
target_include_directories(libblytemu PUBLIC ${RV32EMU_DIR})

# -------------------------------------------------------------------------
# Runtime libraries
# -------------------------------------------------------------------------
add_library(libblytcommon INTERFACE)
add_library(libblytcommonlua INTERFACE)
add_library(libblyt32 INTERFACE)
add_library(libblyt32lua INTERFACE)
add_library(libblytc INTERFACE)

add_library(libblyt STATIC src/libblyt/cart_load.c src/libblyt/cart_run.c)
set_target_properties(libblyt PROPERTIES OUTPUT_NAME blyt)
target_include_directories(
  libblyt
  PUBLIC include
  PRIVATE src/libblyt third_party/flatcc/include "${GENERATED_DIR}"
          ${RV32EMU_DIR})
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
  add_executable(blyt_sdl frontends/sdl/main.c)
  target_include_directories(blyt_sdl PRIVATE include)
  target_link_libraries(blyt_sdl PRIVATE libblyt SDL2::SDL2)
  message(STATUS "SDL2 found via find_package — blyt_sdl will be built")
else()
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(SDL2 QUIET sdl2)
  endif()
  if(SDL2_FOUND)
    add_executable(blyt_sdl frontends/sdl/main.c)
    target_include_directories(blyt_sdl PRIVATE include ${SDL2_INCLUDE_DIRS})
    target_compile_options(blyt_sdl PRIVATE ${SDL2_CFLAGS_OTHER})
    target_link_libraries(blyt_sdl PRIVATE libblyt ${SDL2_LIBRARIES})
    message(STATUS "SDL2 found via pkg-config — blyt_sdl will be built")
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
      add_executable(blyt_sdl frontends/sdl/main.c)
      target_include_directories(blyt_sdl PRIVATE include)
      target_compile_options(blyt_sdl PRIVATE ${SDL2_CFLAGS_LIST})
      target_link_options(blyt_sdl PRIVATE ${SDL2_LDFLAGS_LIST})
      target_link_libraries(blyt_sdl PRIVATE libblyt)
      message(STATUS "SDL2 found via sdl2-config — blyt_sdl will be built")
    else()
      message(STATUS "SDL2 not found — blyt_sdl frontend will not be built")
    endif()
  endif()
endif()

# Frontend stub — libretro (Phase 4)
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
  test_cart_load PRIVATE include src/libblyt third_party/flatcc/include
                         "${TEST_GENERATED_DIR}")
target_link_libraries(test_cart_load PRIVATE libblyt flatccrt)
add_dependencies(test_cart_load flatbuffers_test_generated)
add_test(NAME test_cart_load COMMAND test_cart_load)

# test_cart_run: exercises the ECALL dispatch mechanism using rv32emu directly.
# The test includes common.h explicitly (first), then rv32emu headers; we pass
# the same feature definitions that libblytemu uses.
add_executable(test_cart_run tests/unit/test_cart_run.c)
target_include_directories(test_cart_run PRIVATE include src/libblyt
                                                 ${RV32EMU_DIR})
target_compile_definitions(test_cart_run PRIVATE ${RV32EMU_FEATURE_DEFS})
target_link_libraries(test_cart_run PRIVATE libblytemu)
add_test(NAME test_cart_run COMMAND test_cart_run)
