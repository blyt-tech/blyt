# Doom-shaped host-Lua vs emulated (Lua and native-C) — Pi Zero 2 W

Two Doom-shaped workloads run across the blyt Lua execution models on a real
Raspberry Pi Zero 2 W (Cortex-A53 @ 1 GHz, Debian 13 Trixie, aarch64):

- **host-Lua** — the blyt Lua fork compiled native aarch64 (the host-Lua fast
  path; what a native player would run).
- **emulated Lua** — the same fork compiled RV32 (ilp32d), run under `rv32emu`
  (what `blytplay` runs on an aarch64 handheld today).
- **emulated native-C** — a C twin of the same workload compiled RV32, run under
  the same `rv32emu` — the "write the hot path in C and emulate it" escape hatch.

All from the same algorithm / doubles / fixed-point + the same `BLYT_LUA_I32_F64`
i32 wrap, so **every leg produces the identical integer checksum** (a 3–4-way
determinism cross-check) — the only variable is the execution model.

## Workloads

- **`doom` (`doom_bench.lua` / `doom_c.c`)** — Spike B `doom_tick` (P_Ticker
  game logic): IDLE→CHASE→ATTACK→DEAD state machines, `math.sqrt` range query per
  tic, projectile spawn/expire + LCG kills. One sim = 100 frames × 64 mobs. The
  **branchy game-logic tier.** (The C twin uses a projectile pool vs Lua tables —
  idiomatic each way — but the survivor count, hence checksum, is unchanged.)
- **`draw` (`draw_bench.lua` / `draw_c.c`)** — a Doom `R_DrawColumn` analog:
  affine paletted texture-mapped columns (fixed-point `frac`/`step` texel fetch)
  + colormap lookup → 320×240 framebuffer. The **tight per-pixel renderer tier**
  (all-Lua software renderer — no native primitive to offload to).

## Results (startup-cancelled slope, best-of-N)

| tier | host-Lua | emulated native-C | emulated Lua | host-Lua vs C |
|---|---|---|---|---|
| logic — `doom_tick` | 15.2 ms/sim | 16.7 ms/sim | 811 ms/sim (≈19 MIPS) | **0.91× — tie** |
| draw — `R_DrawColumn` (320×240) | 33.9 ms/frame (~30 fps) | **23.6 ms/frame** | 1638 ms/frame | **1.43× — C wins** |

Checksums: logic 117/sim; draw FNV `1688251611` — identical on all legs.
(M-series Mac desktop reference: host-Lua ~185× / ~180× over emulated Lua.)

## Findings

1. **host-Lua ≈ emulated Lua × ~50×** on both tiers (53× logic, 48× draw) — the
   native-Lua lever Spike Y identifies, on a Doom-shaped workload.
2. **host-Lua vs emulated native-C flips between tiers**, and the pivot is code
   shape via the emulated MIPS:
   - **Logic is branchy/pointer-heavy** → `rv32emu` ~17 MIPS (its worst case).
     C's ~57× fewer instructions are eaten by the low emulated throughput →
     **host-Lua ties emulated-C.** So there is *no reason to drop game logic to a
     C module* on emulated handhelds — stay in Lua at no cost.
   - **Draw is a tight arithmetic inner loop** → `rv32emu` ~31 MIPS (its good
     case). C keeps its ~51× instruction advantage at ~2× the emulated MIPS while
     host-Lua pays Lua's per-pixel table-index overhead → **native-C wins 1.43×**
     (and a *native* renderer, no emulation at all, wins far more).
3. **The measured design split:** Lua sim (host-Lua, as fast as emulated-C) +
   native draw (native primitive / module). host-Lua makes the Lua-sim half free;
   the renderer belongs in native code, as blyt's native-gfx-ECALL model assumes.

## Reproduce

```sh
# repo root, once: cmake -B build -G Ninja   (fetches lua/musl/rv32emu deps)
bash bench/spike-a/scripts/build-host.sh          # native rv32emu runner (this host)
bash bench/spike-a/scripts/build-pi.sh            # aarch64 static runner (for the Pi)
bash bench/spike-a/scripts/build-workload.sh doom # RV32 Lua + native-aarch64 Lua + RV32 C twin
bash bench/spike-a/scripts/build-workload.sh draw

# Pi: host-Lua vs emulated-Lua (slope method)
PI=pizero bash bench/spike-a/scripts/pi-measure.sh doom
PI=pizero bash bench/spike-a/scripts/pi-measure.sh draw
# Pi: emulated native-C leg (run the C twin under the same runner)
#   scp artifacts/guest/{doom,draw}-c.elf pizero:~/doombench/
#   ssh pizero './runner doom-c.elf 550'   # slope vs 50; draw-c.elf 220 vs 20
```
