#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# sharpen-ab.sh [N...]: the compose pass's sharpen (agent.sharpen, compose.c) judged in one scene,
# a debug build in VR mode with the game held still (a stage or a pose): for each N (default
# 0 50 100) cvars.txt gets agent.sharpen=N (read live), and after a full 10 s report window
# drive.py's shot takes the composed left eye (<dir>/sharpen-N.png) and the agent's log its
# compose line, "compose: ... the GPU took T us a frame (longest M); sharpen N, wide W".
# SHARPEN_CVARS: the other lines cvars.txt keeps (default: the palm tracker off; never
# agent.novr2, agent.shotslice); SHARPEN_WIDE=W adds agent.sharpenwide=W. DRIVE_DIR as drive.py's.
HERE=$(cd "$(dirname "$0")" && pwd)
DIR=${DRIVE_DIR:-$HERE/../logs/drive}
BASE=${SHARPEN_CVARS:-"agent.hands=0
agent.errdialog=0"}
[ $# -gt 0 ] || set -- 0 50 100
for n in "$@"; do
  cv="$BASE
agent.sharpen=$n"
  [ -n "$SHARPEN_WIDE" ] && cv="$cv
agent.sharpenwide=$SHARPEN_WIDE"
  # (the first shot carries the new value; its picture may be a frame from before it: dropped)
  DRIVE_CVARS="$cv" python3 "$HERE/drive.py" shot "sharpen-$n-pre" >/dev/null || exit 1
  sleep 22 # (a whole 10 s report window composed at N)
  DRIVE_CVARS="$cv" python3 "$HERE/drive.py" shot "sharpen-$n" || exit 1
  line=$(grep "^compose: .*the GPU took .*sharpen $n," "$DIR/agent.log" | tail -1)
  echo "sharpen $n: ${line:-no compose line at sharpen $n (see $DIR/agent.log)}"
  rm -f "$DIR/sharpen-$n-pre.raw" "$DIR/sharpen-$n-pre.png"
done
