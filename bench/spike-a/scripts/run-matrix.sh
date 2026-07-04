#!/usr/bin/env bash
# Sweep the interpreter optimization level and print effective MIPS per
# benchmark at each. The MIPS cap (ADR-0082) depends strongly on how the shipped
# rv32emu is optimized (~3x between -O0 and -O2), and `cmake -B build` currently
# builds it at -O0 while -O3 needs -fno-strict-aliasing to be correct at all —
# so the "as-shipped" level is a real decision. This produces the evidence.
#
# Usage: run-matrix.sh [--reps N] [--opts "-O0 -O2 -O3"]
# Rebuilds the host runner once per opt level (native to THIS machine).

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

REPS=3
OPTS=(-O0 -O2 -O3)
while [ $# -gt 0 ]; do
  case "$1" in
  --reps)
    REPS="$2"
    shift 2
    ;;
  --opts)
    read -r -a OPTS <<<"$2"
    shift 2
    ;;
  *)
    echo "unknown arg: $1" >&2
    exit 2
    ;;
  esac
done

elfs=("$GUEST_OUT"/*.elf)
[ -e "${elfs[0]}" ] || {
  echo "error: no guest ELFs (run build-guest.sh / build-embench.sh)" >&2
  exit 1
}

log "opt sweep: ${OPTS[*]}   host: $(uname -sm)   reps: $REPS"
echo
# header
printf '%-22s' "benchmark"
for o in "${OPTS[@]}"; do printf '%14s' "MIPS($o)"; done
printf '\n'
printf -- '%.0s-' {1..70}
printf '\n'

median() { sort -n | awk '{a[NR]=$1} END{if(NR%2)print a[(NR+1)/2]; else printf "%.1f",(a[NR/2]+a[NR/2+1])/2}'; }

# Build the runner once per opt, then run all benchmarks; store MIPS in temp files.
tmp="$(mktemp -d)"
for o in "${OPTS[@]}"; do
  SPIKE_A_EMU_OPT="$o" bash "$(dirname "${BASH_SOURCE[0]}")/build-host.sh" >/dev/null 2>&1
  for elf in "${elfs[@]}"; do
    name="$(basename "$elf" .elf)"
    ml=""
    for _ in $(seq 1 "$REPS"); do
      m="$("$ART_DIR/host/runner" "$elf" --json 2>/dev/null |
        sed -n 's/.*"effective_mips":\([0-9.]*\).*/\1/p' || true)"
      ml+="$m"$'\n'
    done
    # `|| true` throughout: a benchmark may abort the runner (rv32emu assertion)
    # → empty input → grep exit 1; don't let pipefail kill the sweep.
    { printf '%s' "$ml" | grep -v '^$' || true; } | median >"$tmp/${name}__${o}" || true
    [ -s "$tmp/${name}__${o}" ] || echo "CRASH" >"$tmp/${name}__${o}"
  done
done

for elf in "${elfs[@]}"; do
  name="$(basename "$elf" .elf)"
  printf '%-22s' "$name"
  for o in "${OPTS[@]}"; do printf '%14s' "$(cat "$tmp/${name}__${o}" 2>/dev/null || echo -)"; done
  printf '\n'
done
rm -rf "$tmp"
echo
log "note: the effective-MIPS number for the ADR-0082 cap is the one measured"
log "      on the Pi Zero 2 W at the interpreter opt level the release build ships."
