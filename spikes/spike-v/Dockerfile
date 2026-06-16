## Spike V — Embedded Swift emitting riscv32 / ilp32d.
##
## Stages covered here (Docker verification path):
##   Stage 0 — trivial Swift → riscv32 object, readelf ABI gate
##   Stage 1 — link against ilp32d guest libs, run in rv32emu
##   Stage 2 — ilp32d ABI witness (Double across extern "C")
##   Stage 3 — ARC + allocator, AMO check
##   Stage 4 — digest equivalence vs C and Rust
##
## Build args:
##   TARGETARCH — set by docker buildx automatically (arm64 or amd64)
##   SWIFT_SNAPSHOT — pinned Swift development snapshot name
##
## Usage (via Makefile):
##   make docker-build-arm64
##   make docker-build-amd64
##   make stage-0        (both hosts)
##   make all

ARG SWIFT_SNAPSHOT=swift-DEVELOPMENT-SNAPSHOT-2026-06-12-a

FROM --platform=${TARGETPLATFORM:-linux/amd64} ubuntu:24.04 AS base

ENV DEBIAN_FRONTEND=noninteractive

# ── System packages + LLVM 22 ─────────────────────────────────────────────────
RUN apt-get update -q \
    && apt-get install -y --no-install-recommends \
       curl ca-certificates gpg \
    && curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key \
       | gpg --dearmor -o /etc/apt/keyrings/apt.llvm.org.gpg \
    && echo "deb [signed-by=/etc/apt/keyrings/apt.llvm.org.gpg] \
       https://apt.llvm.org/noble/ llvm-toolchain-noble-22 main" \
       > /etc/apt/sources.list.d/llvm-22.list \
    && apt-get update -q \
    && apt-get install -y --no-install-recommends \
       llvm-22 clang-22 lld-22 binutils \
       gcc-riscv64-linux-gnu binutils-riscv64-linux-gnu \
       make git python3 \
       libz-dev libncurses6 \
    && update-alternatives \
       --install /usr/bin/clang   clang   /usr/bin/clang-22   100 \
    && update-alternatives \
       --install /usr/bin/lld     lld     /usr/bin/lld-22     100 \
    && update-alternatives \
       --install /usr/bin/llvm-readelf llvm-readelf /usr/bin/llvm-readelf-22 100 \
    && update-alternatives \
       --install /usr/bin/llvm-objdump llvm-objdump /usr/bin/llvm-objdump-22 100 \
    && update-alternatives \
       --install /usr/bin/llvm-size llvm-size /usr/bin/llvm-size-22 100 \
    && rm -rf /var/lib/apt/lists/*

# ── Swift development snapshot ─────────────────────────────────────────────────
# Pinned: swift-DEVELOPMENT-SNAPSHOT-2026-06-12-a
# Two flavours: ubuntu24.04 (amd64) and ubuntu24.04-aarch64 (arm64).
# The TARGETARCH build-arg is set automatically by docker buildx.
ARG SWIFT_SNAPSHOT=swift-DEVELOPMENT-SNAPSHOT-2026-06-12-a
ARG TARGETARCH=amd64

RUN ARCH_SUFFIX="" ; \
    if [ "${TARGETARCH}" = "arm64" ]; then ARCH_SUFFIX="-aarch64"; fi ; \
    SWIFT_PKG="${SWIFT_SNAPSHOT}-ubuntu24.04${ARCH_SUFFIX}" ; \
    SWIFT_URL="https://download.swift.org/development/ubuntu2404${ARCH_SUFFIX}/${SWIFT_SNAPSHOT}/${SWIFT_PKG}.tar.gz" ; \
    echo "Downloading ${SWIFT_URL}" ; \
    curl -fSL "${SWIFT_URL}" -o /tmp/swift.tar.gz \
    && tar -xzf /tmp/swift.tar.gz -C /opt \
    && mv /opt/${SWIFT_PKG} /opt/swift \
    && rm /tmp/swift.tar.gz

ENV PATH="/opt/swift/usr/bin:${PATH}"

# Verify Swift is installed and check version
RUN swiftc --version

# ── Stage 0 — trivial Swift → riscv32 object, ABI gate ───────────────────────
FROM base AS stage0

WORKDIR /spike-v

# Build context is the repo root (Makefile passes -f spikes/spike-v/Dockerfile .).
# COPY from relative paths within the repo.
COPY runtime/guest/include/blyt.h \
     runtime/guest/include/blyt_lua_internal.h \
     /spike-v/include/
COPY spikes/spike-v/trivial.swift \
     spikes/spike-v/cart.swift \
     spikes/spike-v/abi_witness.swift \
     spikes/spike-v/arc_cart.swift \
     spikes/spike-v/probe_cart.swift \
     spikes/spike-v/blyt_bridge.h \
     /spike-v/src/

# Rewrite blyt_bridge.h include path for the Docker layout.
RUN sed -i 's|../../runtime/guest/include/blyt.h|/spike-v/include/blyt.h|' /spike-v/src/blyt_bridge.h

# FV-finding: the correct triple for the pre-built Embedded Swift modules is
# `riscv32-none-none-eabi`, NOT `riscv32-unknown-none-elf` (which the brief
# used as a starting point). The compiler reports the available module triples
# on failure; `riscv32-none-none-eabi` is the RISC-V 32-bit embedded triple.
#
# For F/D/C extensions, -Xfrontend -target-feature does NOT work in this
# snapshot (same error as Apple CLT Swift). The correct mechanism is via
# the LLVM backend attribute pass-through: -Xllvm -mattr=+m,+a,+f,+d,+c
# (LLVM's -mattr syntax). -Xcc -march/mabi affects the Clang importer only.
#
# The ilp32d ABI is specified via -Xcc -mabi=ilp32d (Clang importer) and
# confirmed by the readelf ABI flag on the output object.
RUN swiftc \
      -enable-experimental-feature Embedded \
      -target riscv32-none-none-eabi \
      -Xcc -march=rv32imafdc \
      -Xcc -mabi=ilp32d \
      -Xllvm -mattr=+m,+a,+f,+d,+c \
      -wmo -Osize -parse-as-library \
      -emit-object -o /spike-v/trivial.o \
      /spike-v/src/trivial.swift

# Verify: EF_RISCV_FLOAT_ABI_DOUBLE (flags 0x5) + rv32...f...d...c...
RUN llvm-readelf -h -A /spike-v/trivial.o | tee /spike-v/stage0-readelf.txt
# llvm-readelf-22 renders the flag as "double-float ABI" (flags 0x5), not
# the symbolic name EF_RISCV_FLOAT_ABI_DOUBLE.  Match the actual output.
RUN grep -q "double-float ABI" /spike-v/stage0-readelf.txt \
    && echo "STAGE 0 GATE: double-float ABI (flags 0x5) — PASS" \
    || ( echo "STAGE 0 GATE: double-float ABI — FAIL" ; exit 1 )
# D extension check: arch attr contains d2p2
RUN grep -q "d2p2" /spike-v/stage0-readelf.txt \
    && echo "STAGE 0 GATE: D extension (d2p2) present — PASS" \
    || ( echo "STAGE 0 GATE: D extension not found in arch attr" ; cat /spike-v/stage0-readelf.txt ; exit 1 )

# ── Stage 1 — compile all cart sources with bridging header ──────────────────
# Also used for Stages 2–4: compile each cart source to a separate object file.
# Objects are extracted to the host for linking against the blyt SDK.
#
# -import-bridging-header: Embedded Swift's mechanism for C interop without
# a module map. The bridging header imports blyt.h, making the guest API
# available to Swift cart code.
#
# FV-4 (preliminary): The bridging-header mechanism is Embedded Swift-specific.
# Rust uses extern "C" blocks + bindgen; C++ uses #include directly; C uses
# #include. The bridging-header approach is a per-language extension point —
# the cart API surface (blyt.h) is language-agnostic, but the import mechanism
# is not. Expected extension point, not a leaked assumption.
FROM stage0 AS stage1-compile

WORKDIR /spike-v

# Compile cart.swift (Stage 1 minimal cart)
RUN swiftc \
      -enable-experimental-feature Embedded \
      -target riscv32-none-none-eabi \
      -Xcc -march=rv32imafdc \
      -Xcc -mabi=ilp32d \
      -Xllvm -mattr=+m,+a,+f,+d,+c \
      -import-bridging-header /spike-v/src/blyt_bridge.h \
      -wmo -Osize -parse-as-library \
      -emit-object -o /spike-v/cart.o \
      /spike-v/src/cart.swift

# Compile abi_witness.swift (Stage 2)
RUN swiftc \
      -enable-experimental-feature Embedded \
      -target riscv32-none-none-eabi \
      -Xcc -march=rv32imafdc \
      -Xcc -mabi=ilp32d \
      -Xllvm -mattr=+m,+a,+f,+d,+c \
      -import-bridging-header /spike-v/src/blyt_bridge.h \
      -wmo -Osize -parse-as-library \
      -emit-object -o /spike-v/abi_witness.o \
      /spike-v/src/abi_witness.swift

# Compile arc_cart.swift (Stage 3)
RUN swiftc \
      -enable-experimental-feature Embedded \
      -target riscv32-none-none-eabi \
      -Xcc -march=rv32imafdc \
      -Xcc -mabi=ilp32d \
      -Xllvm -mattr=+m,+a,+f,+d,+c \
      -import-bridging-header /spike-v/src/blyt_bridge.h \
      -wmo -Osize -parse-as-library \
      -emit-object -o /spike-v/arc_cart.o \
      /spike-v/src/arc_cart.swift

# Compile probe_cart.swift (Stage 4)
RUN swiftc \
      -enable-experimental-feature Embedded \
      -target riscv32-none-none-eabi \
      -Xcc -march=rv32imafdc \
      -Xcc -mabi=ilp32d \
      -Xllvm -mattr=+m,+a,+f,+d,+c \
      -import-bridging-header /spike-v/src/blyt_bridge.h \
      -wmo -Osize -parse-as-library \
      -emit-object -o /spike-v/probe_cart.o \
      /spike-v/src/probe_cart.swift
