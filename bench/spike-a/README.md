# Spike A — interpreter-throughput measurement (ADR-0082 MIPS cap)

Measures the **effective guest MIPS** of the blyt rv32emu interpreter on the
minimum emulation host (Raspberry Pi Zero 2 W class — Cortex-A53 @ 1 GHz). That
number is the emulator MIPS cap baked into every emulator build per
[ADR-0082](../../../blyt-planning/docs/adr/console-definition/0082-emulator-mips-cap.md),
and the precondition for the floor-representative per-pixel measurement that
decides the surface-spec Stage-2 Lua fast path (issues #195 / #205).

This is **measurement infrastructure only**. It links the rv32emu core as a
library and runs bare benchmark ELFs through it; it changes nothing under
`runtime/`, adds no cart-visible or determinism surface, and is deliberately
outside the CMake/CTest/CI graph.

## What it measures

```
effective MIPS = retired guest instructions / host wall-clock seconds / 1e6
```

The retired-instruction count is rv32emu's own per-instruction counter
(`rv->csr_cycle`). The runner (`host/runner.c`) loads a benchmark ELF, runs it
to `exit` through the **same interpreter core the runtime ships** (interpreter
only — JIT/T2C/SYSTEM off; Berkeley SoftFloat for F/D), and reports the number.

Benchmarks are cross-compiled to the real blyt cart ISA — **`rv32imafdc` /
`ilp32d`** (note: ADR-0082's "RV32IMFC" predates Spike U adding hardware
doubles) — against static musl, so libm/FP behaviour matches what carts run.

### Benchmarks

- **CoreMark** (EEMBC, Apache-2.0) — the standard embedded CPU benchmark; gives
  a widely-comparable score plus the headline MIPS. Barebones port + static
  musl; the emulation reproduces CoreMark's canonical validation CRCs.
- **Embench-IoT** — a more representative embedded suite (19 kernels), as a
  cross-check.

## The one methodology caveat: interpreter optimization level

The measured MIPS depends **strongly** (~3×) on how the rv32emu core itself is
compiled:

| interpreter opt | note |
|---|---|
| `-O0` | what `cmake -B build` currently produces (no `CMAKE_BUILD_TYPE` set) |
| `-O2` | a sane release interpreter — the harness default |
| `-O3` | works, but no gain over `-O2` (occasionally marginally slower) |

Any optimized build (`-O2` **and** `-O3`) requires `-fno-strict-aliasing`, or
rv32emu miscompiles and the guest starts at `PC=0` (see the finding in
`RESULTS.md`); the harness always sets it. So the cap value is only meaningful
paired with a decision about the opt level the *shipped* emulator uses.
`run-matrix.sh` sweeps the levels; `RESULTS.md` records the measured numbers.

## Layout

```
bench/spike-a/
  host/
    runner.c            measurement runner (loads ELF, runs via rv32emu, reports MIPS)
    CMakeLists.txt      self-contained build (rv32emu subset + softfloat + runner)
  guest/
    coremark-port/      CoreMark port layer (core_portme.c)
    embench-port/       Embench board-support port
  scripts/
    lib.sh              shared config + toolchain discovery
    build-musl.sh       build static musl for rv32imafdc/ilp32d
    build-guest.sh      build the CoreMark guest ELF
    build-embench.sh    build the Embench guest ELFs
    build-host.sh       build the runner natively (this Mac)
    build-pi.sh         cross-build a static runner for the Pi (linux/arm64 Docker)
    run.sh              run the guest ELFs through a runner, print a MIPS table
    run-matrix.sh       sweep interpreter opt levels
  artifacts/            build outputs (gitignored)
  .deps/                fetched/built deps: musl, coremark, embench (gitignored)
  RESULTS.md            measured numbers + findings
```

## Usage

### On this Mac (dev/comparison baseline)

```sh
cd bench/spike-a
scripts/build-guest.sh          # builds static musl (~1 min first time) + coremark.elf
scripts/build-embench.sh        # builds embench-*.elf
scripts/build-host.sh           # builds artifacts/host/runner (interpreter -O2)
scripts/run.sh                  # prints the effective-MIPS table
scripts/run-matrix.sh           # MIPS across interpreter -O0/-O2/-O3
```

Requires Homebrew `llvm` + `lld` (for the rv32 cross-compile). Override the
toolchain with `BLYT_CLANG` / `BLYT_LLD` if needed.

### On the Raspberry Pi Zero 2 W (the authoritative floor)

Cross-build the runner from the Mac (needs Docker), then copy it plus the
prebuilt guest ELFs to the Pi — the Pi needs no toolchain:

```sh
# on the Mac:
scripts/build-guest.sh && scripts/build-embench.sh   # host-independent ELFs
scripts/build-pi.sh                                   # -> artifacts/pi/runner (aarch64 static)
scp -r artifacts/pi/runner artifacts/guest scripts pi@pi:spike-a/

# on the Pi:
cd spike-a && ./scripts/run.sh --runner ./runner
```

The effective-MIPS figure from the Pi run, at the interpreter opt level the
release build ships, is the ADR-0082 cap. Record it in `RESULTS.md`.
