#!/bin/sh
# route-photo.sh [TITLE]: route-vr.sh, then on into a PHOTO PARADISE pose scene (Scarlet 1.08):
# VR Paradise tutorial panels 1-4 (touch pad each; circle first), the VR PARADISE menu (Event /
# Pictorial / Photo Paradise / VR Tutorial): Photo Paradise, Kasumi (its help panel shows only the
# first time ever, then is saved as seen: not pressed for), the Basic
# Standing pose card -> stage picker, right (the next stage after Niki Beach, morning, by the sea), the swimsuit
# picker (the first suit), up onto the film strip, circle plays the pose in VR.
d=$(dirname "$0")
t() { python3 "$d/drive.py" tap "$1" "${3:-150}" > /dev/null; sleep "$2"; }
sh "$d/route-vr.sh" "$@" || exit 1
t circle 5
t touch 6 200; t touch 6 200; t touch 6 200; t touch 6 200
t down 3; t down 3; t circle 6
t circle 6
t circle 6
t right 3; t circle 12
t circle 15
t up 4
t circle 15
python3 "$d/drive.py" shot route-scene | tail -1
