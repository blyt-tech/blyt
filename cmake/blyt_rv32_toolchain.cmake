# cmake/blyt_rv32_toolchain.cmake
#
# Configure-time discovery of a riscv32-capable clang/lld toolchain, shared by
# the guest-library build rules (cmake/blyt_guest_libs.cmake) and the sdk
# assembly script (cmake/blyt_sdk.cmake, which receives the results as -D
# arguments from the sdk target).
#
# Sets (empty string when unavailable): BLYT_RV32_CLANG / BLYT_RV32_CLANGPP /
# BLYT_RV32_LLD BLYT_RV32_OBJCOPY / BLYT_RV32_AR / BLYT_RV32_LLDB_DAP
#
# Resolution order: 1. Known candidate clang/lld pairs, verified with a riscv32
# probe compile. 2. A previously downloaded toolchain at
# ${CMAKE_BINARY_DIR}/sdk/toolchain. 3. If BLYT_RV32_FETCH=ON: download an LLVM
# release into sdk/toolchain. (OFF by default so a plain configure never
# triggers a large download; guest libraries and the SDK are skipped with a hint
# instead.)

set(BLYT_RV32_CLANG "")
set(BLYT_RV32_CLANGPP "")
set(BLYT_RV32_LLD "")
set(BLYT_RV32_OBJCOPY "")
set(BLYT_RV32_AR "")
set(BLYT_RV32_LLDB_DAP "")

option(BLYT_RV32_FETCH
       "Download an LLVM release at configure time if no riscv32 clang found"
       OFF)

set(_BLYT_TOOLCHAIN_DIR "${CMAKE_BINARY_DIR}/sdk/toolchain")

# ── 1. Candidate pairs ───────────────────────────────────────────────────────
set(_CANDIDATE_PAIRS
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
    "/usr/local/bin/ld.lld"
    # A toolchain downloaded by a previous configure / sdk run
    "${_BLYT_TOOLCHAIN_DIR}/bin/clang"
    "${_BLYT_TOOLCHAIN_DIR}/bin/ld.lld")

list(LENGTH _CANDIDATE_PAIRS _PAIR_LEN)
math(EXPR _PAIR_COUNT "${_PAIR_LEN} / 2")
math(EXPR _PAIR_LAST "${_PAIR_COUNT} - 1")

foreach(_IDX RANGE ${_PAIR_LAST})
  math(EXPR _CI "${_IDX} * 2")
  math(EXPR _LI "${_IDX} * 2 + 1")
  list(GET _CANDIDATE_PAIRS ${_CI} _CAND_CLANG)
  list(GET _CANDIDATE_PAIRS ${_LI} _CAND_LLD)
  if(NOT EXISTS "${_CAND_CLANG}" OR NOT EXISTS "${_CAND_LLD}")
    continue()
  endif()
  execute_process(
    COMMAND "${_CAND_CLANG}" --target=riscv32 -march=rv32imafdc -mabi=ilp32d -x c
            -E - -o /dev/null
    INPUT_FILE /dev/null
    RESULT_VARIABLE _RV32_OK
    ERROR_QUIET OUTPUT_QUIET)
  if(_RV32_OK EQUAL 0)
    set(BLYT_RV32_CLANG "${_CAND_CLANG}")
    set(BLYT_RV32_LLD "${_CAND_LLD}")
    break()
  endif()
endforeach()

# ── 2. Download (opt-in) ─────────────────────────────────────────────────────
if(NOT BLYT_RV32_CLANG AND BLYT_RV32_FETCH)
  set(_LLVM_VER "22.1.5")
  if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    if(CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL "arm64")
      set(_LLVM_TRIPLE "arm64-apple-macos11")
    else()
      set(_LLVM_TRIPLE "x86_64-apple-darwin")
    endif()
  elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    if(CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL "aarch64")
      set(_LLVM_TRIPLE "aarch64-linux-gnu")
    else()
      set(_LLVM_TRIPLE "x86_64-linux-gnu-ubuntu-24.04")
    endif()
  else()
    message(FATAL_ERROR "Unsupported host platform '${CMAKE_HOST_SYSTEM_NAME}' "
                        "for the LLVM toolchain download.")
  endif()

  set(_LLVM_ARCHIVE "clang+llvm-${_LLVM_VER}-${_LLVM_TRIPLE}.tar.xz")
  set(_LLVM_URL
      "https://github.com/llvm/llvm-project/releases/download/llvmorg-${_LLVM_VER}/${_LLVM_ARCHIVE}"
  )
  set(_LLVM_ARCHIVE_PATH "${CMAKE_BINARY_DIR}/downloads/${_LLVM_ARCHIVE}")

  if(NOT EXISTS "${_LLVM_ARCHIVE_PATH}")
    message(STATUS "Downloading LLVM ${_LLVM_VER} (${_LLVM_TRIPLE})…")
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/downloads")
    file(
      DOWNLOAD "${_LLVM_URL}" "${_LLVM_ARCHIVE_PATH}"
      SHOW_PROGRESS
      STATUS _DL_STATUS)
    list(GET _DL_STATUS 0 _DL_CODE)
    if(NOT _DL_CODE EQUAL 0)
      file(REMOVE "${_LLVM_ARCHIVE_PATH}")
      message(FATAL_ERROR "LLVM download failed: ${_DL_STATUS}")
    endif()
  endif()

  message(STATUS "Extracting LLVM toolchain…")
  file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/sdk")
  execute_process(
    COMMAND ${CMAKE_COMMAND} -E tar xf "${_LLVM_ARCHIVE_PATH}"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/sdk"
    RESULT_VARIABLE _EXTRACT_RESULT)
  if(NOT _EXTRACT_RESULT EQUAL 0)
    message(FATAL_ERROR "LLVM toolchain extraction failed")
  endif()
  file(GLOB _EXTRACTED_DIR "${CMAKE_BINARY_DIR}/sdk/clang+llvm-*")
  if(NOT _EXTRACTED_DIR)
    message(FATAL_ERROR "Could not find extracted LLVM directory")
  endif()
  file(RENAME "${_EXTRACTED_DIR}" "${_BLYT_TOOLCHAIN_DIR}")

  set(BLYT_RV32_CLANG "${_BLYT_TOOLCHAIN_DIR}/bin/clang")
  set(BLYT_RV32_LLD "${_BLYT_TOOLCHAIN_DIR}/bin/ld.lld")
endif()

# ── 3. Companion tools ───────────────────────────────────────────────────────
if(BLYT_RV32_CLANG)
  get_filename_component(_TOOL_DIR "${BLYT_RV32_CLANG}" DIRECTORY)
  get_filename_component(_CLANG_NAME "${BLYT_RV32_CLANG}" NAME)
  # Prefer the versioned companion tools (e.g. clang++-22 for clang-22) so we
  # don't accidentally pick up a different version via the generic symlink.
  string(REGEX MATCH "-[0-9]+$" _VER_SUFFIX "${_CLANG_NAME}")
  if(_VER_SUFFIX AND EXISTS "${_TOOL_DIR}/clang++${_VER_SUFFIX}")
    set(BLYT_RV32_CLANGPP "${_TOOL_DIR}/clang++${_VER_SUFFIX}")
  elseif(EXISTS "${_TOOL_DIR}/clang++")
    set(BLYT_RV32_CLANGPP "${_TOOL_DIR}/clang++")
  endif()
  if(_VER_SUFFIX AND EXISTS "${_TOOL_DIR}/llvm-objcopy${_VER_SUFFIX}")
    set(BLYT_RV32_OBJCOPY "${_TOOL_DIR}/llvm-objcopy${_VER_SUFFIX}")
  elseif(EXISTS "${_TOOL_DIR}/llvm-objcopy")
    set(BLYT_RV32_OBJCOPY "${_TOOL_DIR}/llvm-objcopy")
  endif()
  if(_VER_SUFFIX AND EXISTS "${_TOOL_DIR}/llvm-ar${_VER_SUFFIX}")
    set(BLYT_RV32_AR "${_TOOL_DIR}/llvm-ar${_VER_SUFFIX}")
  elseif(EXISTS "${_TOOL_DIR}/llvm-ar")
    set(BLYT_RV32_AR "${_TOOL_DIR}/llvm-ar")
  endif()
  if(_VER_SUFFIX AND EXISTS "${_TOOL_DIR}/lldb-dap${_VER_SUFFIX}")
    set(BLYT_RV32_LLDB_DAP "${_TOOL_DIR}/lldb-dap${_VER_SUFFIX}")
  elseif(EXISTS "${_TOOL_DIR}/lldb-dap")
    set(BLYT_RV32_LLDB_DAP "${_TOOL_DIR}/lldb-dap")
  endif()
  message(STATUS "RV32 toolchain: clang = ${BLYT_RV32_CLANG}")
else()
  message(
    STATUS "RV32 toolchain: not found — guest libraries and the SDK will be \
unavailable. Install LLVM with riscv32 support (Homebrew llvm+lld / apt \
clang-22 lld-22) or reconfigure with -DBLYT_RV32_FETCH=ON to download one.")
endif()
