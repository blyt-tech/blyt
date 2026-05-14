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

# FlatBuffers codegen — shared between C runtime and Rust packer (ADR-0126)
find_program(FLATC flatc REQUIRED)
message(STATUS "Found flatc: ${FLATC}")

set(SCHEMAS_DIR "${CMAKE_SOURCE_DIR}/schemas")
set(GENERATED_DIR "${CMAKE_BINARY_DIR}/generated/flatbuffers")
file(MAKE_DIRECTORY "${GENERATED_DIR}")

add_custom_command(
    OUTPUT
        "${GENERATED_DIR}/cart_info_generated.h"
        "${GENERATED_DIR}/cart_config_generated.h"
    COMMAND "${FLATC}" --cpp -o "${GENERATED_DIR}"
        "${SCHEMAS_DIR}/cart_info.fbs"
        "${SCHEMAS_DIR}/cart_config.fbs"
    DEPENDS
        "${SCHEMAS_DIR}/cart_info.fbs"
        "${SCHEMAS_DIR}/cart_config.fbs"
    COMMENT "Generating FlatBuffers headers"
    VERBATIM
)

add_custom_target(flatbuffers_generated ALL
    DEPENDS
        "${GENERATED_DIR}/cart_info_generated.h"
        "${GENERATED_DIR}/cart_config_generated.h"
)

# Runtime libraries — empty stubs for Phase 1.
# Will be converted to STATIC/SHARED targets as source is added.
add_library(libblytcommon    INTERFACE)
add_library(libblytcommonlua INTERFACE)
add_library(libblyt32        INTERFACE)
add_library(libblyt32lua     INTERFACE)
add_library(libblytc         INTERFACE)
add_library(libblyt          INTERFACE)

# Frontend stubs — Phase 1
add_library(blyt_libretro INTERFACE)
