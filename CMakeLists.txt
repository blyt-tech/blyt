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

# Runtime libraries
add_library(libblytcommon INTERFACE)
add_library(libblytcommonlua INTERFACE)
add_library(libblyt32 INTERFACE)
add_library(libblyt32lua INTERFACE)
add_library(libblytc INTERFACE)

add_library(libblyt STATIC src/libblyt/cart_load.c)
set_target_properties(libblyt PROPERTIES OUTPUT_NAME blyt)
target_include_directories(
  libblyt
  PUBLIC include
  PRIVATE src/libblyt third_party/flatcc/include "${GENERATED_DIR}")
add_dependencies(libblyt flatbuffers_generated)

# Frontend stubs — Phase 1
add_library(blyt_libretro INTERFACE)

# Tests
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
