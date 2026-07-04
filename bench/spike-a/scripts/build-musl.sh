#!/usr/bin/env bash
# Build a static musl (libc.a + crt1/crti/crtn) for the blyt cart ISA
# (rv32imafdc / ilp32d) using the blyt clang toolchain. Cached in .deps.
#
# musl provides the C runtime (malloc/mallocng, string, libm, printf, CRT
# startup) for the benchmark ELFs. Using the same pinned blyt-tech/musl and
# clang the runtime uses keeps libm/FP behaviour faithful to real carts.

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
require_guest_toolchain
guest_flags

MUSL_SRC="$DEPS_DIR/musl-src"
MUSL_INSTALL="$DEPS_DIR/musl-install"

if [ -f "$MUSL_INSTALL/lib/libc.a" ] && [ "${1:-}" != "--force" ]; then
  log "static musl already built at $MUSL_INSTALL (use --force to rebuild)"
  exit 0
fi

if [ ! -d "$MUSL_SRC" ]; then
  log "fetching musl $MUSL_VERSION"
  tmp="$DEPS_DIR/musl.tar.gz"
  curl -fsSL "$MUSL_URL" -o "$tmp"
  mkdir -p "$MUSL_SRC"
  tar xzf "$tmp" -C "$MUSL_SRC" --strip-components=1
  rm -f "$tmp"
fi

log "configuring musl ($GUEST_MARCH / $GUEST_MABI)"
build="$DEPS_DIR/musl-build"
rm -rf "$build"
cp -R "$MUSL_SRC" "$build"
cd "$build"
CC="$CLANG --target=$GUEST_TRIPLE -march=$GUEST_MARCH -mabi=$GUEST_MABI -fuse-ld=$LLD" \
  AR="$LLVM_AR" RANLIB="$LLVM_RANLIB" \
  ./configure --target=riscv32 --disable-shared --prefix="$MUSL_INSTALL" >/dev/null

log "building musl (this takes ~1 min)"
make -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)" >/dev/null
make install >/dev/null
log "static musl installed at $MUSL_INSTALL"
