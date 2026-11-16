#!/bin/sh
# step.sh NAME BUTTON [WAIT_S [HOLD_MS]]: tools/drive.py tap BUTTON, wait, then a shot NAME (none: shot only)
d=$(dirname "$0")
[ "$2" != none ] && python3 "$d/drive.py" tap "$2" "${4:-200}" > /dev/null
sleep "${3:-4}"
python3 "$d/drive.py" shot "$1" | tail -1
