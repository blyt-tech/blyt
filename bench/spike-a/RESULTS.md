# Spike A — results

Effective guest MIPS of the blyt rv32emu interpreter core, measured by running
CoreMark / Embench ELFs (RV32IMAFDC / ilp32d, static musl) through the same
interpreter the runtime ships. See `README.md` for method.

> **The authoritative cap value is the Pi Zero 2 W number, at the interpreter
> optimization level the release build ships.** Fill in the Pi column by running
> `scripts/build-pi.sh` on a Mac and `scripts/run.sh --runner ./runner` on the
> Pi. The Mac numbers below are the dev/comparison baseline.

## Correctness (sanity before speed)

CoreMark through the interpreter reproduces the canonical CoreMark validation
CRCs, confirming the emulation is correct, not just fast:

```
seedcrc 0xe9f5   crclist 0xe714   crcmatrix 0x1fd7   crcstate 0x8e3a
```

Guest instruction counts are deterministic (same ELF + interpreter → identical
`csr_cycle`), e.g. CoreMark @ 2000 iterations = 604,481,314 guest instructions
on every run; only wall-clock (hence MIPS) varies.

## Effective MIPS — dev Mac (Apple Silicon M-series), baseline

Interpreter core built with the mandatory `-fno-strict-aliasing`. This is the
dev/comparison baseline — **not** the cap (the cap is the Pi number).

### Full suite at interpreter `-O2`, CoreMark @ 2000 iterations, gsf=100

| benchmark | guest_insns | effective MIPS |
|---|---:|---:|
| coremark | 604,484,389 | **550** |
| embench-crc32 | 400,562,444 | 1045 |
| embench-tarfind | 118,053,257 | 758 |
| embench-nettle-aes | 351,231,957 | 801 |
| embench-matmult-int | 127,227,795 | 728 |
| embench-md5sum | 258,430,370 | 691 |
| embench-depthconv | 273,228,971 | 647 |
| embench-ud | 263,128,094 | 620 |
| embench-statemate | 187,830,680 | 613 |
| embench-qrduino | 289,051,029 | 586 |
| embench-huffbench | 249,822,455 | 581 |
| embench-xgboost | 342,137,257 | 566 |
| embench-picojpeg | 260,269,788 | 555 |
| embench-wikisort | 74,590,328 | 538 |
| embench-nettle-sha256 | 518,404,634 | 523 |
| embench-sglib-combined | 266,330,190 | 511 |
| embench-edn | 349,144,162 | 497 |
| embench-slre | 289,875,623 | 493 |
| embench-nsichneu | 200,330,637 | 272 |
| embench-aha-mont64 | — | crashes rv32emu (see finding 4) |

CoreMark also self-reports **1804 iterations/sec** at -O2. The suite clusters
around ~500–650 MIPS with control-flow-heavy `nsichneu` low (272) and tight
integer `crc32` high (1045).

### Opt-level sensitivity — `scripts/run-matrix.sh` (effective MIPS)

The interpreter opt level moves the number ~3.4× consistently across the whole
suite; `-O3` gives no gain over `-O2` (occasionally slightly worse).

| benchmark | -O0 | -O2 | -O3 |
|---|---:|---:|---:|
| coremark | 158 | 545 | 522 |
| embench-crc32 | 178 | 1026 | 988 |
| embench-nettle-aes | 224 | 799 | 760 |
| embench-matmult-int | 205 | 734 | 734 |
| embench-md5sum | 189 | 693 | 684 |
| embench-depthconv | 190 | 694 | 723 |
| embench-ud | 163 | 620 | 614 |
| embench-statemate | 153 | 616 | 618 |
| embench-qrduino | 176 | 585 | 580 |
| embench-huffbench | 167 | 583 | 573 |
| embench-xgboost | 169 | 562 | 553 |
| embench-picojpeg | 160 | 557 | 555 |
| embench-wikisort | 140 | 539 | 547 |
| embench-nettle-sha256 | 163 | 517 | 513 |
| embench-sglib-combined | 138 | 502 | 501 |
| embench-edn | 170 | 499 | 496 |
| embench-slre | 123 | 499 | 495 |
| embench-tarfind | 158 | 729 | 775 |
| embench-nsichneu | 62 | 274 | 270 |
| _suite mean_ | _~170_ | _~600_ | _~590_ |

Same trend on the `hello` microbench (tight 5-insn loop): -O0 ≈ 129, -O2 ≈ 414,
-O3 ≈ 399.

## aarch64 build validation (not the cap)

The Pi runner (`build-pi.sh`, aarch64 static) was executed in a `linux/arm64`
container against the guest ELFs: clean halt, zero unhandled syscalls, correct
CoreMark CRCs, and Embench guest-instruction counts **bit-identical** to the Mac
(e.g. crc32 400,562,188; nsichneu 200,330,381; matmult-int 127,227,539) —
confirming the cross-built binary runs the benchmarks correctly. (CoreMark's
count varies by ~300 across hosts because its printed elapsed-time string has
different digits → slightly different `ee_printf` work; Embench prints no timing
and is exactly deterministic.) The container MIPS (~532 for CoreMark) is **not**
Pi-representative — arm64 Docker on Apple Silicon runs near-native.

## Effective MIPS — Raspberry Pi Zero 2 W (authoritative floor)

Measured on real hardware: **Raspberry Pi Zero 2 W Rev 1.0, Cortex-A53 @
1000 MHz, aarch64, Debian 13 (Trixie), kernel 6.18.34+rpt-rpi-v8** — exactly the
ADR-0082 minimum emulation host. Runner cross-built from the Mac
(`build-pi.sh`), aarch64 static; guest ELFs identical to the Mac run
(instruction counts match). CoreMark @ 2000 iterations runs 18.8 s on the Pi, so
its official ≥10 s gate passes: **"Correct operation validated", CRCs correct**.

The Pi is ~17× slower than the dev Mac at a given interpreter opt level.

| benchmark | MIPS @ interp -O2 | MIPS @ interp -O0 |
|---|---:|---:|
| coremark | **32.1** | **7.9** |
| embench-crc32 | 56.7 | 6.5 |
| embench-nettle-aes | 42.2 | 9.4 |
| embench-tarfind | 40.8 | 8.9 |
| embench-depthconv | 40.3 | 8.8 |
| embench-qrduino | 38.4 | 8.8 |
| embench-matmult-int | 37.4 | 8.7 |
| embench-md5sum | 36.1 | 8.5 |
| embench-nettle-sha256 | 34.1 | 8.5 |
| embench-ud | 33.4 | 8.3 |
| embench-xgboost | 32.7 | 8.2 |
| embench-huffbench | 32.4 | 7.9 |
| embench-picojpeg | 32.0 | 8.1 |
| embench-statemate | 30.9 | 7.7 |
| embench-edn | 30.2 | 7.2 |
| embench-sglib-combined | 28.4 | 7.5 |
| embench-wikisort | 27.5 | 7.2 |
| embench-slre | 23.6 | 6.8 |
| embench-nsichneu | 12.8 | 4.0 |
| _suite: CoreMark + cluster_ | _~13–57, cluster ~30–40_ | _~4–9, cluster ~7–9_ |
| embench-aha-mont64 | crashes rv32emu | crashes rv32emu |

`-O3` matches `-O2` on the Pi (CoreMark 32.9 vs 32.1). CoreMark self-reports
106.2 iters/sec at -O2 and 108.7 at -O3, both "Correct operation validated".

### The Lua bytecode interpreter runs ~36% slower than CoreMark (Spike B probe)

CoreMark and Embench are native-C workloads. Lua carts run a *second* layer of
interpretation — the Lua VM (compiled to RV32) dispatched by rv32emu — which is
the load-bearing authoring path. `bench/spike-a/guest/lua-port/` builds the
**blyt Lua 5.4 VM** (int32/float64 `BLYT_LUA_I32_F64`, fixed hash seed, + the
guest quad soft-float builtins) to the cart ISA and runs a steady-state entity
`update()` (256 entities, position/velocity integration, `sqrt`/`sin` per
entity). Deterministic: identical digest and instruction count on Mac and Pi.

| effective MIPS | CoreMark | Lua-VM | Lua ÷ CoreMark |
|---|---:|---:|---:|
| Pi, interp **-O2** | 32.1 | **20.5** | **64 %** |
| Pi, interp -O0 | 7.9 | 6.7 | 85 % |
| Mac, interp -O2 | 545 | 438 | 80 % |

At the recommended `-O2` interpreter, the Lua VM sustains only **~20 MIPS on the
Pi** — 36 % below CoreMark. The gap is *widest* at `-O2` (at `-O0` rv32emu's
per-instruction overhead dominates everything, so workload mix matters less; at
`-O2` the Lua VM's indirect-dispatch + softfloat-f64 mix shows through, and the
in-order A53 punishes it more than Apple Silicon does — 64 % vs 80 %).

**Consequence for the cap:** effective MIPS is workload-dependent, and a cap set
from CoreMark (32) is **~1.57× optimistic for Lua carts**. A Lua cart tuned to
fit a 32-MIPS dev throttle (~535K guest insns/frame) gets only ~342K/frame on
real hardware → dropped frames on the Pi. Since Lua is a first-class authoring
language, the cap should account for this — either anchor it to the Lua-VM
number (~20 MIPS, conservative for all cart types), or use a per-execution-model
cap (native-C carts capped ~32, Lua carts ~20). This is Spike B territory; the
probe here quantifies it so the ADR-0082 cap decision is informed by the
worst-case authoring path, not just native C.

### Per-frame budget and GC jitter on hardware (Spike B)

The runner records a per-frame instruction distribution when the guest emits a
frame marker (a private `SYS_BLYT_FRAME` ecall each frame); converted to time at
the measured MIPS, that gives the direct 60 Hz (16.67 ms) budget verdict — and
GC pauses land in the high percentiles. Two workloads (`lua-bench.elf <frames>
[steady|alloc]`), 256 entities, **Pi at -O2**:

| workload | insns/frame (mean / p99 / max) | ms/frame (mean / p99 / max) | vs 16.67 ms |
|---|---|---|---|
| **steady** (no per-frame alloc) | 737K / 744K / 744K | 36.5 / 36.9 / 36.9 | 2.2× over |
| **alloc** (table+string/entity/frame) | 1.26M / 1.62M / 1.62M | 70.4 / 90.3 / 90.3 | 4.2–5.4× over |

Reading:
- **256 entities of this complexity does not fit** — 36.5 ms/frame steady (2.2×
  over). The Pi Lua budget at -O2 is 16.67 ms × ~20.5 MIPS ≈ **342K Lua-VM
  instructions/frame** (full frame), or ~**171K** to leave the spike's ≥ 8 ms
  headroom. At ~2.9K insns/entity that is **~60 entities (with headroom) to ~120
  (full budget)** — a non-trivial but bounded retro loop. Lighter per-entity
  logic (no `sqrt`/`sin`) buys proportionally more.
- **GC matters.** The steady frame is rock-stable (p99 ≈ mean). The
  allocation-heavy frame both runs slower *and* shows **p99 ≈ 28 % above mean**
  (1.62M vs 1.26M insns) — Lua's mark/sweep pauses. A cart must budget to the
  **p99**, not the mean, and minimize per-frame allocation to hold a steady 60 Hz.

This closes the load-bearing part of Spike B (throughput + fps/headroom + GC on
real hardware). Remaining Spike B breadth: the lua.org benchmark suite for
operation-type coverage (string/closure/numeric).

### The cap value — and why it hinges on the interpreter opt level

**The shipped interpreter optimization level swings the cap ~4× on the Pi:**

| interpreter opt | CoreMark MIPS | per-frame budget @ 60 Hz (16.67 ms) |
|---|---:|---:|
| `-O0` — what `cmake -B build` produces **today** | **7.9** | ~133K guest insns/frame |
| `-O2` — a sane optimized release | **32.1** | ~536K guest insns/frame |
| `-O3` | 32.9 | ~549K guest insns/frame |

- If the runtime ships **as it builds today (-O0)**, the cap is **~8 effective
  guest MIPS** (~133K guest instructions per frame). That is a *very* tight
  budget — likely below the spike's success criterion (a non-trivial retro game
  loop leaving ≥ half the frame budget for subsystem overhead).
- If the runtime ships an **optimized (-O2)** interpreter, the cap is **~32
  effective guest MIPS** (~536K guest instructions per frame) — a comfortable
  retro-era budget (draws are native-speed ECALLs, not counted here).

**Recommendation:** bake the cap **only after deciding two things** — the
release interpreter opt level, and whether the cap is one number or
per-execution-model:

1. Ship the emulator core built `-O2 -fno-strict-aliasing` (the `-O0` the repo
   builds today would set the cap ~4× too low, ≈ 8 MIPS).
2. On that basis native-C carts sit at **≈ 32 effective guest MIPS**
   (CoreMark-anchored). **But Lua carts only sustain ≈ 20 MIPS** (see the Lua
   probe above), so a single 32-MIPS cap is ~1.57× optimistic for the primary
   authoring path. Either set the cap conservatively at **≈ 20 MIPS** (safe for
   every cart type) or make it **per-execution-model** (native ≈ 32, Lua ≈ 20).

If the interpreter stays at `-O0`, the cap is ≈ 8 MIPS (Lua ≈ 7) and the
floor-hardware performance story needs re-examination regardless.

| benchmark | Pi MIPS (opt=?) |
|---|---|
| coremark | _tbd_ |
| embench-* | _tbd_ |

## Findings

1. **The cap value is opt-level-dependent (~3.4× on the Mac, 4× on the Pi).**
   Effective MIPS swings ~4× on the Pi between an `-O0` and an `-O2` interpreter
   (CoreMark 7.9 vs 32.1). ADR-0082's cap is only well-defined once the *shipped*
   interpreter opt level is pinned. Today `cmake -B build` sets no
   `CMAKE_BUILD_TYPE`, so the interpreter (and blytplay) build at `-O0` — so the
   cap would be set ~4× too low (~8 MIPS) relative to an optimized shipping build
   (~32 MIPS). See "The cap value" above; this is the headline decision the spike
   surfaces.

2. **Any optimized rv32emu build needs `-fno-strict-aliasing`.** At `-O2` *and*
   `-O3`, the interpreter core silently miscompiles (guest starts at
   `PC=0x00000000`, "failed to allocate or translate block") unless
   `-fno-strict-aliasing` is set — rv32emu reads/writes guest memory through
   incompatible pointer types, which the type-based alias analysis enabled at
   `-O2`+ breaks. The flag makes it well-defined (same reason the Linux kernel
   uses it); the harness always sets it. `-O3` is otherwise fine but buys nothing
   over `-O2` for an interpreter dispatch loop (see the opt grid above —
   occasionally marginally slower). Latent risk for any future move to an
   optimized runtime build.

3. **The interpreter is workload-sensitive but stable.** At `-O2` the 19-kernel
   Embench suite spans ~272 (control-flow-heavy `nsichneu`) to ~1045 MIPS
   (tight-integer `crc32`), clustered ~500–650, with CoreMark at 550 — a single
   cap number is representative for native C, and the workload spread is bounded
   and understood.

3a. **The Lua bytecode interpreter runs ~36 % slower than CoreMark on the Pi**
   (20.5 vs 32.1 MIPS at `-O2`) — see "The Lua bytecode interpreter…" above. A
   CoreMark-anchored cap is ~1.57× optimistic for the primary authoring path, so
   the cap should be set from the Lua-VM number or made per-execution-model. (A
   Spike B probe built off this harness.)

4. **`aha-mont64` deterministically aborts the rv32emu interpreter.** It builds
   and links fine but trips an assertion in rv32emu's block/constant optimizer
   (`assert(rv->X[0] == 0)`, `emulate.c:1837`, in `optimize_constant`) partway
   through execution. This is inside the vendored rv32emu core (not the harness
   or a syscall gap), so a real cart with a similar instruction stream could hit
   it too — worth a follow-up against the emulator. The other 18 Embench kernels
   and CoreMark run clean (halted, zero unhandled syscalls, verify PASS). The
   harness reports it as `CRASHED` rather than hiding it.
