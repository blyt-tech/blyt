# Doom-shaped host-Lua vs emulated — Pi Zero 2 W

Two Doom-shaped Lua workloads run **native-aarch64 (host-Lua leg)** vs
**RV32-under-rv32emu (emulated leg)** on a real Raspberry Pi Zero 2 W
(Cortex-A53 @ 1 GHz, Debian 13 Trixie, aarch64). Same blyt Lua fork
(`BLYT_LUA_I32_F64`, fixed hash seed `0x424C5954`) and same embedded Lua — only
native-vs-emulated differs. Corroborates Spike Y's ~44–58× native-Lua lever on a
realistic game shape, and adds a render-determinism cross-check.

## Workloads

- **`doom` (`doom_bench.lua`)** — the Spike B `doom_tick` P_Ticker slice:
  IDLE→CHASE→ATTACK→DEAD state machines, a `math.sqrt` range query per tic,
  projectile `table.insert`/`table.remove` GC churn, LCG kills. One sim = 100
  frames × 64 mobs. The **game-logic tier**.
- **`draw` (`draw_bench.lua`)** — a Doom `R_DrawColumn` analog: affine paletted
  texture-mapped vertical columns (fixed-point `frac`/`step` texel fetch) + a
  colormap light lookup, written to a 320×240 Lua framebuffer. The **all-Lua
  software-renderer (per-pixel) tier** — the case with no native primitive to
  offload to.

Both are pure integer/fixed-point, so the checksum (logic: live-state sum;
draw: framebuffer FNV-1a) is bit-identical across legs — a determinism check on
top of the timing.

## Results (startup-cancelled slope, best-of-N)

| tier | native host-Lua | emulated rv32emu | speedup |
|---|---|---|---|
| logic — `doom_tick` | 15.2 ms/sim | 811 ms/sim (≈19 MIPS) | **53×** |
| draw — `R_DrawColumn` (320×240) | 33.9 ms/frame (~30 fps) | 1638 ms/frame (~0.6 fps) | **48×** |

Checksums: logic 117/sim; draw FNV `1688251611`. Emulated rv32emu clocks
~19 MIPS (logic) / ~25 MIPS (draw), matching Spike A/B's Pi floor.

Reference (M-series Mac desktop, same benchmarks): ~185× (logic), ~180× (draw)
— the out-of-order core widens the gap the in-order A53 narrows to the ~50×
floor.

## Reading

Both tiers land in the same ~50× band because both are pure Lua-VM work. A Doom
cart therefore forks:

- **All-Lua renderer** (texture mapping in Lua): the whole frame runs at ~50× —
  ~30 fps native vs ~0.6 fps emulated per full-screen pass. Emulated unplayable;
  host-Lua borderline-playable.
- **Native renderer module** (idiomatic at Doom scale): draw is native both legs
  (leg-neutral); host-Lua's 53× applies to the logic tier — a ~50× larger Lua
  game-logic budget.

Either way the lever is: run Lua native (Spike Y's conclusion).

## Reproduce

```sh
# from repo root, once: cmake -B build -G Ninja   (fetches lua/musl/rv32emu deps)
bash bench/spike-a/scripts/build-host.sh            # native rv32emu runner (this host)
bash bench/spike-a/scripts/build-pi.sh              # aarch64 static runner (for the Pi)
bash bench/spike-a/scripts/build-workload.sh doom   # RV32 ELF + aarch64 native binary
bash bench/spike-a/scripts/build-workload.sh draw
PI=pizero bash bench/spike-a/scripts/pi-measure.sh doom
PI=pizero bash bench/spike-a/scripts/pi-measure.sh draw
```
