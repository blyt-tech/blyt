#!/bin/sh
# Spike Y repeat — time blytplay's per-frame cost for a set of carts. Runs ON the
# target device (no ssh in the measurement loop).
#
#   ./bench-player.sh <blytplay> <cart.blyt>...
#
# blytplay --headless free-runs (it only paces to 60 Hz when a window or a dev
# control channel is attached), so wall-clock/frame is the raw update()+draw()
# cost. Startup (cart load, VM boot, guest lib dlopen on the emulated leg) is
# removed with a two-point slope: t(N2) - t(N1) over N2 - N1 frames.
#
# Frame counts are chosen adaptively so each point runs ~4 s regardless of
# whether a frame costs 20 us (host-Lua) or 500 ms (emulated). Three rounds; the
# minimum is reported (least scheduler noise) alongside the median.
set -eu
PLAYER="$1"; shift
ROUNDS="${ROUNDS:-3}"
TARGET_US="${TARGET_US:-4000000}"   # ~4 s per timing point

t_run() {   # t_run <frames> <cart> -> elapsed microseconds
  _n="$1"; _c="$2"
  _t0=$(date +%s%N)
  "$PLAYER" --headless --quit-after "$_n" "$_c" >/dev/null 2>&1
  _t1=$(date +%s%N)
  echo $(( (_t1 - _t0) / 1000 ))
}

printf '%-12s %10s %10s %8s %10s\n' cart min_ms/frame med_ms/frame frames px/16.67ms
for cart in "$@"; do
  name=$(basename "$cart" .blyt)

  # calibrate: 2 and 10 frames -> rough per-frame cost
  c1=$(t_run 2 "$cart"); c2=$(t_run 10 "$cart")
  slope=$(( (c2 - c1) / 8 ))
  [ "$slope" -lt 1 ] && slope=1
  n2=$(( TARGET_US / slope ))
  [ "$n2" -lt 20 ] && n2=20
  [ "$n2" -gt 30000 ] && n2=30000
  n1=$(( n2 / 5 ))
  [ "$n1" -lt 4 ] && n1=4

  results=""
  r=0
  while [ "$r" -lt "$ROUNDS" ]; do
    a=$(t_run "$n1" "$cart"); b=$(t_run "$n2" "$cart")
    results="$results $(( (b - a) * 1000 / (n2 - n1) ))"   # nanoseconds/frame
    r=$(( r + 1 ))
  done

  echo "$results" | awk -v name="$name" -v n2="$n2" '{
    n = NF; for (i = 1; i <= n; i++) v[i] = $i
    for (i = 1; i < n; i++) for (j = i + 1; j <= n; j++) if (v[j] < v[i]) { t = v[i]; v[i] = v[j]; v[j] = t }
    min = v[1] / 1e6; med = v[int((n + 1) / 2)] / 1e6
    printf "%-12s %10.3f %10.3f %8d %10s\n", name, min, med, n2, "-"
  }'
done
