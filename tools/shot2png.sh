#!/bin/sh
# shot2png.sh RAW [SCALE]: agent.shot's raw copy to pictures beside it (tools/detile, sips).
# By its size: the flat display buffer (1920x1080: RAW's .png, and -thin.png, the same bytes read as
# the VR mode's flat texture, which is the same size in the other tiling), or the eye array of VR
# mode (four 940x1052 slices at pitch 1024: -wideL, -wideR, -nearL, -nearR .png).
d=$(dirname "$0")
[ -x "$d/detile" ] && [ "$d/detile" -nt "$d/detile.c" ] || cc -O2 -o "$d/detile" "$d/detile.c" || exit 1
b=${1%.raw}
png() { sips -s format png "$1.ppm" --out "$1.png" > /dev/null 2>&1; rm -f "$1.ppm"; }
size=$(wc -c < "$1" | tr -d ' ')
if [ "$size" -eq 18874368 ]; then
  i=0
  for v in wideL wideR nearL nearR; do
    "$d/detile" "$1" "$b-$v.ppm" 940 1052 16 "${2:-2}" thin 1024 $((i * 4718592)) $i 2> /dev/null && png "$b-$v"
    i=$((i + 1))
  done
else
  "$d/detile" "$1" "$b.ppm" 1920 1080 16 "${2:-2}" display 2> /dev/null && png "$b"
  "$d/detile" "$1" "$b-thin.ppm" 1920 1080 16 "${2:-2}" thin 2> /dev/null && png "$b-thin"
fi
