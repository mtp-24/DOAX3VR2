#!/bin/sh
# route-vr.sh [TITLE]: the game started and taken into VR Paradise without a player (tools/drive.py).
# The start-up boxes (the autosave notice; Fortune's shop-data message over the title, offline)
# are skipped by the payload unless agent.bootnotes=1 (logs/research/boot/NOTES.md), so by default:
#   both games: "press any button" (circle), then down twice to VR PARADISE and circle.
# With agent.bootnotes=1 in DRIVE_CVARS, a circle for each box first:
#   Fortune 1.19: autosave notice, shop-data message, "press any button" (circle each).
#   Scarlet 1.08: autosave notice, "press any button" (circle each).
# A circle too many is not harmless: on the main menu it chooses the highlighted item.
# (Scarlet's opening movie plays only on a first boot without save data; Fortune's was seen.)
# A shot at the menu pauses to check VR PARADISE is highlighted (logs/drive/route-menu.png).
# DRIVE_CVARS as drive.py's (default: an observing run).
d=$(dirname "$0")
t() { python3 "$d/drive.py" tap "$1" "${3:-200}" > /dev/null; sleep "$2"; }
python3 "$d/drive.py" start "${1:-CUSA04555}" || exit 1
sleep 42
case "${DRIVE_CVARS:-}" in
*agent.bootnotes=1*)
  t circle 5 # the autosave notice
  if [ "${1:-CUSA04555}" = CUSA04555 ]; then
    t circle 4 # Fortune's shop-data message over its title
  fi
  ;;
esac
t circle 6 # "press any button": the main menu
t down 3 150
t down 3 150
sleep 3
python3 "$d/drive.py" shot route-menu | tail -1
sleep 2
t circle 12
python3 "$d/drive.py" log 6
