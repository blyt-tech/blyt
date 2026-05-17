# cmake/blyt_sdk.cmake
#
# Assembles a self-contained blyt SDK under build/sdk/:
#
#   build/sdk/
#     bin/        blyt (devtool), blytrun (SDL2 runtime)
#     include/    blyt.h, blyt32.h, blyt_runtime.h
#     lib/        libblytcommon.so, libblyt32.so  (RV32IMAFC)
#     toolchain/  clang, ld.lld, llvm-objcopy, …
#
# Invoked via `cmake --build build --target sdk`.
# Required variables (injected by the sdk target in CMakeLists.txt):
#   BLYT_SOURCE_DIR, BLYT_BINARY_DIR,
#   CMAKE_HOST_SYSTEM_NAME, CMAKE_HOST_SYSTEM_PROCESSOR

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
    "/opt/homebrew/opt/llvm/bin/clang" "/opt/homebrew/opt/llvm/bin/ld.lld"
    "/opt/homebrew/opt/llvm/bin/clang" "/opt/homebrew/opt/llvm/bin/lld"
    "/usr/bin/clang"                   "/usr/bin/ld.lld"
    "/usr/local/bin/clang"             "/usr/local/bin/ld.lld")

set(FOUND_CLANG "")
set(FOUND_LLD "")
set(FOUND_OBJCOPY "")

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
    ERROR_QUIET
    OUTPUT_QUIET)
  if(RV32_OK EQUAL 0)
    set(FOUND_CLANG "${CAND_CLANG}")
    set(FOUND_LLD "${CAND_LLD}")
    get_filename_component(TOOL_DIR "${CAND_CLANG}" DIRECTORY)
    if(EXISTS "${TOOL_DIR}/llvm-objcopy")
      set(FOUND_OBJCOPY "${TOOL_DIR}/llvm-objcopy")
    endif()
    break()
  endif()
endforeach()

if(NOT FOUND_CLANG)
  # Download a pre-built LLVM release.
  set(LLVM_VER "18.1.8")
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
      set(LLVM_TRIPLE "x86_64-linux-gnu-ubuntu-22.04")
    endif()
  else()
    message(
      FATAL_ERROR
      "Unsupported host platform '${CMAKE_HOST_SYSTEM_NAME}'. "
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
      file(DOWNLOAD "${LLVM_URL}" "${LLVM_ARCHIVE_PATH}"
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
      message(FATAL_ERROR "Could not find extracted LLVM directory in ${SDK_DIR}")
    endif()
    file(RENAME "${EXTRACTED_DIR}" "${SDK_TOOLCHAIN}")
  endif()

  set(FOUND_CLANG "${SDK_TOOLCHAIN}/bin/clang")
  set(FOUND_LLD "${SDK_TOOLCHAIN}/bin/ld.lld")
  set(FOUND_OBJCOPY "${SDK_TOOLCHAIN}/bin/llvm-objcopy")
endif()

message(STATUS "blyt SDK: toolchain clang = ${FOUND_CLANG}")

# -------------------------------------------------------------------------
# Step 2: Build RV32IMAFC runtime libraries
# -------------------------------------------------------------------------

file(MAKE_DIRECTORY "${SDK_LIB}")

set(RV32_BASE
    --target=riscv32
    -march=rv32imafc
    -mabi=ilp32f
    -shared
    -fPIC
    -nostdlib
    # -Wl,--shared explicitly tells lld to produce ET_DYN; clang may inject
    # -Bstatic for bare-metal riscv targets which overrides -shared otherwise.
    -Wl,--shared
    -I "${BLYT_SOURCE_DIR}/include")
if(FOUND_LLD)
  list(APPEND RV32_BASE "-fuse-ld=${FOUND_LLD}")
endif()

message(STATUS "Building libblytcommon.so…")
# --allow-undefined: blyt_cart_init/update/draw are provided by the cart at
# runtime (reverse symbol lookup); lld requires explicit permission for this.
execute_process(
  COMMAND "${FOUND_CLANG}" ${RV32_BASE} -Wl,-soname,libblytcommon.so
          -o "${SDK_LIB}/libblytcommon.so"
          "${BLYT_SOURCE_DIR}/src/libblytcommon/blyt_common.c"
  RESULT_VARIABLE R)
if(NOT R EQUAL 0)
  message(FATAL_ERROR "Failed to build libblytcommon.so")
endif()

message(STATUS "Building libblyt32.so…")
# libblyt32.so is self-contained: it statically absorbs libblytcommon.so's
# source so the cart's DT_NEEDED is only {libblyt32.so}.  libblytcommon.so
# remains a separate library for cross-variant code that links blyt.h directly.
execute_process(
  COMMAND "${FOUND_CLANG}" ${RV32_BASE} -Wl,-soname,libblyt32.so -o
          "${SDK_LIB}/libblyt32.so" "${BLYT_SOURCE_DIR}/src/libblyt32/blyt32.c"
          "${BLYT_SOURCE_DIR}/src/libblytcommon/blyt_common.c"
  RESULT_VARIABLE R)
if(NOT R EQUAL 0)
  message(FATAL_ERROR "Failed to build libblyt32.so")
endif()

# -------------------------------------------------------------------------
# Step 3: SDK headers
# -------------------------------------------------------------------------

file(MAKE_DIRECTORY "${SDK_INC}")
file(
  COPY "${BLYT_SOURCE_DIR}/include/blyt.h"
       "${BLYT_SOURCE_DIR}/include/blyt32.h"
       "${BLYT_SOURCE_DIR}/include/blyt_runtime.h"
  DESTINATION "${SDK_INC}")

# -------------------------------------------------------------------------
# Step 4: blyt devtool
# -------------------------------------------------------------------------

file(MAKE_DIRECTORY "${SDK_BIN}")
execute_process(
  COMMAND cargo build --manifest-path
          "${BLYT_SOURCE_DIR}/devtool/Cargo.toml"
  RESULT_VARIABLE R)
if(NOT R EQUAL 0)
  message(FATAL_ERROR "Failed to build blyt devtool")
endif()
file(COPY "${BLYT_SOURCE_DIR}/devtool/target/debug/blyt" DESTINATION "${SDK_BIN}")

# Expose the SDK's own toolchain binaries under blyt-prefixed names in bin/.
# Using blyt-lld / blyt-objcopy avoids any collision with host tools and lets
# blyt build use -fuse-ld=blyt-lld reliably across platforms.
if(EXISTS "${SDK_TOOLCHAIN}/bin/ld.lld")
  file(CREATE_LINK "${SDK_TOOLCHAIN}/bin/ld.lld" "${SDK_BIN}/blyt-lld" SYMBOLIC)
  # Also keep ld.lld for direct invocation
  file(CREATE_LINK "${SDK_TOOLCHAIN}/bin/ld.lld" "${SDK_BIN}/ld.lld" SYMBOLIC)
endif()
if(EXISTS "${SDK_TOOLCHAIN}/bin/llvm-objcopy")
  file(CREATE_LINK "${SDK_TOOLCHAIN}/bin/llvm-objcopy" "${SDK_BIN}/blyt-objcopy"
       SYMBOLIC)
  file(CREATE_LINK "${SDK_TOOLCHAIN}/bin/llvm-objcopy" "${SDK_BIN}/llvm-objcopy"
       SYMBOLIC)
endif()
if(EXISTS "${SDK_TOOLCHAIN}/bin/clang")
  file(CREATE_LINK "${SDK_TOOLCHAIN}/bin/clang" "${SDK_BIN}/blyt-clang" SYMBOLIC)
endif()

# -------------------------------------------------------------------------
# Step 5: blytrun (if built)
# -------------------------------------------------------------------------

if(EXISTS "${BLYT_BINARY_DIR}/blytrun")
  file(COPY "${BLYT_BINARY_DIR}/blytrun" DESTINATION "${SDK_BIN}")
endif()

# -------------------------------------------------------------------------
# Summary
# -------------------------------------------------------------------------

message(STATUS "blyt SDK assembled at ${SDK_DIR}")
message(STATUS "  bin:       ${SDK_BIN}")
message(STATUS "  include:   ${SDK_INC}")
message(STATUS "  lib:       ${SDK_LIB}")
if(EXISTS "${SDK_TOOLCHAIN}")
  message(STATUS "  toolchain: ${SDK_TOOLCHAIN}")
endif()
message(STATUS "")
message(STATUS "Build a cart:  BLYT_SDK_DIR=${SDK_DIR} ${SDK_BIN}/blyt build <project>")
