# Building

Needs the [PS5 payload SDK](https://github.com/ps5-payload-dev/sdk), LLVM (clang and
ld.lld; Homebrew's on macOS) and Python 3:

    export PS5_PAYLOAD_SDK=~/ps5sdk/opt/ps5-payload-sdk
    export LLVM_CONFIG=/opt/homebrew/opt/llvm/bin/llvm-config   # macOS with Homebrew's LLVM
    make            # a debug build (the tools below)
    make RELEASE=1  # the one to hand out

`LLVM_CONFIG` is read by the SDK's own compiler wrapper (`prospero-llvm-config`), which finds
its LLVM through it; without it the payload's build stops at the SDK's clang.

The output is `payload/doax3vr2.elf`; the in-game part (`agent/`) is built first
and embedded in it. The agent's Makefile takes clang from `LLVM` (default
`/opt/homebrew/opt/llvm/bin`) and ld.lld from `LLD`; the payload's from the SDK's
toolchain. A release build keeps only the player's settings and the settings menu
([SETTINGS.md](SETTINGS.md)), a short log, and none of the debug tools: the debug menu
(which takes the settings menu's place in a debug build), the test mode and test-runner
commands on the control port, and the measuring options are left out, and it always
runs its built-in agent.

The GPU shaders' headers (`agent/*_gcn.h`) are checked in; rebuilding them needs LLVM's
AMDGPU tools (`llvm-mc`, from `LLVM_MC` or Homebrew's LLVM; `tools/gcn_asm.py`,
`tools/gcn_cc.py`).

## The source

- `agent/` runs inside the game: the PS VR emulation and the game-side features (the
  package files, the item map, the shops, the fan, the malfunction flags: `hooks.c`; `vr.c`),
  the PS VR2 output and tracking (`vr2.c`), the eyes composed from the wide and near views
  (`compose.c`), the GPU compatibility layer (`gen1.c`, `acb.c`), the PS5-library fixes
  (`ps5compat.c`), the TV picture (`mirror.c`), hand tracking (`hand.c`), the settings menu
  and the debug menu (`menu.c`) with its pass into the headset's picture (`inset.c`, the
  shader `tools/gpu/inset.c`), the test mode's controller (`pads.c`), the per-thread timing
  (`rtime.c`), the eye tracking (`fov.c`) and the moving sharp area (`gaze.c`), the view sizes
  (`viewsize.c`), Real height (`height.c`), touch (`touch.c`) and Look at you and the 120 fps
  timers (`vrreact.c`).
- `payload/` is the loader: it builds the libraries and the shader on first run
  (`prepare.c`, `rdna2gcn.c`), puts the agent into the game (`main.c`, `pt.c`) and
  patches the game's code at its entry point ([HOW-IT-WORKS.md](HOW-IT-WORKS.md) 3).
- `common/` is shared by both: the firmware profiles (`firmware.h`), the test mode and
  the headset cameras' state.
- `tools/` holds the drive and test scripts ([TESTING.md](TESTING.md)), the address finder
  for other builds of a game (`ebootmap/`), `psarc_pull.py` (files from a game's
  archive on the console), the shader tools (the compose and menu shaders: `gpu/compose.c`,
  `gpu/inset.c`),
  the host tests (`fov/`), the hand-tracker gate's tests (`hand/`) and the menu font's
  generator (`font/`).

## Host tests

The host tests in `tools/fov/` build with the host's `cc` and need no console; each
takes `-I../../agent`. `menu_test` draws the settings menu to a PNG:

    cd tools/fov
    cc -O2 -I../../agent -o /tmp/menutest menu_test.c -lz && /tmp/menutest /tmp/menu.png
    cc -O2 -DRELEASE -I../../agent -o /tmp/menutest-r menu_test.c -lz && /tmp/menutest-r /tmp/menu-r.png
    cc -O2 -I../../agent -o /tmp/rt rtime_test.c && /tmp/rt

The hand-tracker gate's tests in `tools/hand/` and the compose shader's CPU reference
(`tools/gpu/compose_ref.sh`) give their build lines at the top of each file.
