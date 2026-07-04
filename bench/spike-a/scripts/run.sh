#!/usr/bin/env bash
# Run the benchmark guest ELFs through a Spike A runner and print an
# effective-MIPS table. Reports the median of N repetitions (guest instruction
# counts are deterministic; wall-clock — hence MIPS — is not).
#
# Usage:
#   run.sh [--runner PATH] [--reps N] [--show-bench-output]
#
# Defaults: runner = artifacts/host/runner (this host), reps = 3.
# On the Pi, point --runner at the cross-built artifacts/pi/runner.

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

RUNNER="$ART_DIR/host/runner"
REPS=3
SHOW_BENCH=0
while [ $# -gt 0 ]; do
  case "$1" in
  --runner)
    RUNNER="$2"
    shift 2
    ;;
  --reps)
    REPS="$2"
    shift 2
    ;;
  --show-bench-output)
    SHOW_BENCH=1
    shift
    ;;
  *)
    echo "unknown arg: $1" >&2
    exit 2
    ;;
  esac
done

[ -x "$RUNNER" ] || {
  echo "error: runner not found/executable: $RUNNER (run build-host.sh)" >&2
  exit 1
}
elfs=("$GUEST_OUT"/*.elf)
[ -e "${elfs[0]}" ] || {
  echo "error: no guest ELFs in $GUEST_OUT (run build-guest.sh)" >&2
  exit 1
}

host_desc="$(uname -sm)"
log "runner: $RUNNER"
log "host:   $host_desc   reps: $REPS"
printf '\n%-20s %14s %12s  %s\n' "benchmark" "guest_insns" "MIPS(med)" "wall(med,s)"
printf -- '---------------------------------------------------------------\n'

median() { # stdin: numbers -> median
  sort -n | awk '{a[NR]=$1} END{if(NR%2)print a[(NR+1)/2]; else printf "%.3f",(a[NR/2]+a[NR/2+1])/2}'
}

for elf in "${elfs[@]}"; do
  name="$(basename "$elf" .elf)"
  mips_list=""
  wall_list=""
  insns=""
  halted="true"
  unhandled=0
  crashed=0
  for _ in $(seq 1 "$REPS"); do
    # `|| true`: a benchmark can abort the runner (rv32emu assertion); don't let
    # set -e kill this script — we detect and report the crash below.
    if [ "$SHOW_BENCH" = 1 ]; then
      json="$("$RUNNER" "$elf" --json 2>/dev/null | tail -1 || true)"
    else
      json="$("$RUNNER" "$elf" --json 2>/dev/null || true)"
    fi
    # Empty/garbled JSON => the runner itself aborted (e.g. an rv32emu assertion
    # on a pathological instruction stream). Report it rather than a blank row.
    if ! printf '%s' "$json" | grep -q '"effective_mips"'; then
      crashed=1
      break
    fi
    m="$(printf '%s' "$json" | sed -n 's/.*"effective_mips":\([0-9.]*\).*/\1/p')"
    w="$(printf '%s' "$json" | sed -n 's/.*"wall_seconds":\([0-9.]*\).*/\1/p')"
    insns="$(printf '%s' "$json" | sed -n 's/.*"guest_insns":\([0-9]*\).*/\1/p')"
    h="$(printf '%s' "$json" | sed -n 's/.*"halted":\([a-z]*\).*/\1/p')"
    u="$(printf '%s' "$json" | sed -n 's/.*"unhandled_syscalls":\([0-9]*\).*/\1/p')"
    [ "$h" = "false" ] && halted="false"
    [ -n "$u" ] && [ "$u" != "0" ] && unhandled="$u"
    mips_list+="$m"$'\n'
    wall_list+="$w"$'\n'
  done
  if [ "$crashed" = 1 ]; then
    printf '%-20s %14s %12s  %s\n' "$name" "-" "-" "CRASHED (runner aborted)"
    continue
  fi
  mmed="$(printf '%s' "$mips_list" | grep -v '^$' | median)"
  wmed="$(printf '%s' "$wall_list" | grep -v '^$' | median)"
  flag=""
  [ "$halted" = "false" ] && flag+=" NOT-HALTED"
  [ "$unhandled" != "0" ] && flag+=" UNHANDLED-SYSCALLS($unhandled)"
  printf '%-20s %14s %12s  %s%s\n' "$name" "$insns" "$mmed" "$wmed" "$flag"
done
echo
