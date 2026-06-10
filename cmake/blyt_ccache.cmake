# cmake/blyt_ccache.cmake
#
# Compiler-launcher caching via ccache.  When ccache is on PATH (and
# -DBLYT_CCACHE is not OFF), every CMake-driven compile goes through it: the
# host tree (this lists file, including the riscv64 cross configure of it) and —
# via BLYT_CCACHE_PROGRAM passed to cmake/blyt_sdk.cmake — the nested libc++ and
# WASM (emcc) trees.  ccache replays bit-identical compiler output for identical
# inputs, so determinism is unaffected.
#
# Not covered here: the RV32 guest libs (single compile+link mega-commands in
# cmake/blyt_guest_libs.cmake — not cacheable by ccache) and Rust (cargo's own
# caching applies).
#
# Honors a pre-set CMAKE_C[XX]_COMPILER_LAUNCHER so users can override.

option(BLYT_CCACHE "Use ccache as the compiler launcher when available" ON)

set(BLYT_CCACHE_PROGRAM "")
if(BLYT_CCACHE)
  find_program(CCACHE_PROGRAM ccache)
  if(CCACHE_PROGRAM)
    set(BLYT_CCACHE_PROGRAM "${CCACHE_PROGRAM}")
    if(NOT CMAKE_C_COMPILER_LAUNCHER)
      set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    endif()
    if(NOT CMAKE_CXX_COMPILER_LAUNCHER)
      set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    endif()
    message(STATUS "ccache: ${CCACHE_PROGRAM}")
  else()
    message(STATUS "ccache: not found (compiles will not be cached)")
  endif()
endif()
