#!/usr/bin/env bash
# Deploy the built workload artifacts to a Pi and measure host-Lua (native)
# vs emulated (rv32emu runner) with the startup-cancelled slope method.
# Env: PI=<ssh-host> (default pizero). Run build-pi.sh + build-workload.sh <name> first.
#   pi-measure.sh doom   |   pi-measure.sh draw
set -euo pipefail
NAME="${1:?usage: pi-measure.sh <doom|draw>}"; PI="${PI:-pizero}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/artifacts"
ssh "$PI" 'mkdir -p ~/doombench'
scp -q "$ROOT/pi/runner" "$ROOT/pi/lua-${NAME}-native" "$ROOT/guest/lua-${NAME}.elf" "$PI:~/doombench/"
# native reps: fast; emulated reps: ~50x slower — small counts, slope cancels startup.
case "$NAME" in
  doom) NL=200 NH=1200 EL=3  EH=13 ;;
  draw) NL=20  NH=120  EL=1  EH=4  ;;
esac
ssh "$PI" "cd ~/doombench && chmod +x runner lua-${NAME}-native
 nat(){ local best='' o; for i in 1 2 3; do local s=\$(date +%s.%N); o=\$(./lua-${NAME}-native \$1); local e=\$(date +%s.%N)
   best=\$(echo \"\$best \$(echo \"\$e-\$s\"|bc)\"|tr ' ' '\n'|grep .|sort -g|head -1); done; echo \"\$best|\$o\"; }
 emu(){ local best='' l; for i in 1 2; do l=\$(./runner lua-${NAME}.elf \$1 2>&1)
   best=\$(echo \"\$best \$(echo \"\$l\"|grep -o 'wall=[0-9.]*'|tr -d 'wal=s')\"|tr ' ' '\n'|grep .|sort -g|head -1); done; echo \"\$best\"; }
 NA=\$(nat $NL); NB=\$(nat $NH); EA=\$(emu $EL); EB=\$(emu $EH)
 echo \"native  $NL/$NH: \${NA%|*} \${NB%|*}s  \${NA#*|}\"
 echo \"emulated $EL/$EH: \$EA \$EB s\"
 python3 - \"\${NA%|*}\" \"\${NB%|*}\" \"\$EA\" \"\$EB\" <<PY
import sys; n1,n2,e1,e2=map(float,sys.argv[1:5]); hs=(n2-n1)/($NH-$NL); es=(e2-e1)/($EH-$EL)
print(f'native host-Lua: {hs*1e3:8.2f} ms/unit'); print(f'emulated rv32  : {es*1e3:8.2f} ms/unit')
print(f'SPEEDUP: {es/hs:.0f}x')
PY"
