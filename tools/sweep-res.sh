#!/bin/sh
# sweep-res.sh TITLE WIDTH...: the eye size swept on VR Paradise's opening scene, without a player.
# For each width (or "stock"): resolution.txt uploaded, the game started and routed into VR
# (route-vr.sh), 45 s measured, a picture taken, the game stopped (the payload stays resident).
# The run's part of the agent's log goes to logs/sweep/<title>-<width>.log, its picture beside it.
d=$(dirname "$0")
title=$1; shift
out="$d/../logs/sweep"; mkdir -p "$out"
for w in "$@"; do
  python3 - "$d" "$w" <<'EOF' || exit 1
import io, sys
sys.path.insert(0, sys.argv[1])
import drive
f = drive.ftp()
f.storbinary("STOR %s/resolution.txt" % drive.CONS, io.BytesIO((sys.argv[2] + "\n").encode()))
f.quit()
EOF
  sh "$d/${ROUTE:-route-vr.sh}" "$title" > "$out/$title-$w$SWEEP_TAG.route" 2>&1 || { echo "$w: the start failed"; cat "$out/$title-$w$SWEEP_TAG.route"; exit 1; }
  cp "$d/../logs/drive/route-menu.png" "$out/$title-$w$SWEEP_TAG-menu.png" 2>/dev/null
  sleep 45
  python3 "$d/drive.py" shot "sweep-$w" > /dev/null 2>&1
  cp "$d/../logs/drive/sweep-$w.png" "$out/$title-$w$SWEEP_TAG.png" 2>/dev/null
  python3 "$d/drive.py" stop > /dev/null 2>&1
  awk '/resolution: /{n=NR} {l[NR]=$0} END{for(i=n;i<=NR;i++) print l[i]}' "$d/../logs/drive/agent.log" > "$out/$title-$w$SWEEP_TAG.log"
  echo "== $title $w $SWEEP_TAG"
  grep -E 'resolution: |compose: each eye|dmem: flexible' "$out/$title-$w$SWEEP_TAG.log" | tail -3
  grep -E 'hmd: [0-9]+ frames handed over|compose: [0-9]+ frames composed|hmd: the game drew' "$out/$title-$w$SWEEP_TAG.log" | tail -6
  sleep 40
done
