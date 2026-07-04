# #208 Stage 2 — Lua fast-pixel spike: results & parked VM-patch prototypes

Spike question: is tier-2 Lua per-pixel access (`blyt32.surface.acquire` →
`lk:get`/`lk:set`) fast enough with a plain C method binding (2a), or does it
need a Lua-VM fast path (2b)? If 2b, which candidate?

## Decision (2026-07-04): **ship 2a only. Do not productionize a VM patch.**

On the real emulated floor (Pi Zero 2 W, the ADR-0002/0082 device), even the
fastest VM patch does not get Lua per-pixel into the target 1k–10k px/frame
range. Bulk per-pixel belongs in C / tier-1 primitives; Lua per-pixel is for
modest counts (tens–low-hundreds px/frame), where 2a (plain method, shipped in
PR #213) is already adequate.

## Measurements (ms/frame)

Dev Mac (x86 emulating rv32):

```
                 1k px    4k px    10k px
2a  method       10.5     42.6     100.9
2b  OP_CALL       3.9     15.0      35.6
2b  index         2.1      7.6      18.1
C   ceiling        —       0.4        —
```

**Pi Zero 2 W (aarch64, the real floor — ~15–16× slower):**

```
                 1k px    4k px    10k px      µs/px
2a  method      163.3    629.5    1525.0       ~154
2b  OP_CALL      71.5    263.3     630.9        ~64
2b  index        37.4    126.1     295.0        ~31
C   ceiling        —       8.9        —         ~1
```

Max pixels/frame within the full 16.67 ms @ 60 Hz budget on the Pi (0 left for
game logic): 2a ~108, OP_CALL ~260, **index ~540**, C ~17,000. The use-case bar
(≤ ~8.3 ms, half the budget) roughly halves those. So even the winning VM patch
is ~2× short of the 1k low end while consuming the whole frame.

Spike-A byproduct: Pi per-pixel floor ≈ 154 µs/px (plain Lua) / 31 µs/px (best
VM patch) / ~1 µs/px (C).

## The two candidates (parked prototypes)

- **Candidate 2 — OP_CALL-inline**: recognizes the `lk:set`/`lk:get` light-C
  builtins at `OP_CALL` and runs the pixel op inline, eliding the call frame.
  Keeps the clean method API; naturally coherent (falls through to the C
  function). ~2.7× over 2a.
- **Candidate 3 — index-opcode `lk[i]`**: intercepts `OP_GETTABLE`/`OP_SETTABLE`
  for the lock userdata + integer key. Also kills the `OP_SELF` method lookup →
  ~2× faster than candidate 2 (~5.3× over 2a), the fastest. But: worse DX
  (linear index + stride, needs a `blyt.pset/pget` build-time macro) and NOT
  coherent as-is — `lk[i]` only works where the VM is patched, so it needs an
  `__index`/`__newindex` metamethod fallback for the host-Lua WASM leg.

Both are bit-identical to the slow path (pure speed optimizations, guest-only via
`-DBLYT_LUA_FASTPIXEL`), proven across all legs incl. the QEMU gate.

## Why parked (not deleted)

On the *emulated* floor the patches don't cross the viability threshold, so
they're not worth the VM-fork + coherence complexity today. But **native
RISC-V hardware (the K230D target) has no emulation overhead** — the per-pixel
cost there is closer to the C ceiling, so a VM fast path could become worthwhile.
Revisit these then. Candidate 3 (index) is the one to resurrect (fastest).

## How to reapply

```sh
# 1. Lua fork: clone blyt-tech/lua into third_party/lua, base tag v5.5.0-blyt-v0-p1
git clone https://github.com/blyt-tech/lua third_party/lua
git -C third_party/lua checkout blyt-patches-v0
git -C third_party/lua apply /path/to/01-lua-fork-lvm-fastpixel.patch

# 2. blyt side (cmake option + fast-path identities), base commit c889b29
git apply 02-blyt-side-fastpixel.patch

# 3. configure — NOTE the repo's _blyt_fc_local macro sets the WRONG-CASE
#    FETCHCONTENT_SOURCE_DIR_lua; pass the UPPERCASE var explicitly, fresh build:
rm -rf build && cmake -B build -G Ninja -DFETCHCONTENT_SOURCE_DIR_LUA="$PWD/third_party/lua"
# BLYT_LUA_FASTPIXEL defaults ON; -DBLYT_LUA_FASTPIXEL=OFF builds the 2a baseline lib.
```

For CI (not just local), the fork changes need a pre-release tag on blyt-tech/lua
(`v5.5.0-blyt-v0-p1-<feature>`) + a `LUA_VERSION` URL/hash bump in CMakeLists.txt.

Gotchas hit while prototyping (all in the patches): `lua_pushcfunction` registers
0-upvalue **light C functions** (`ttislcf`/`fvalue`, not `ttisCclosure`); the
fast-path epoch/identities must be **non-static** so `lvm.c` reads the same
symbols; the lock userdata mirror struct in `lvm.c` must match `lua_lock_t`
(pixels/stride/w/h/token/epoch/released) on ilp32.
