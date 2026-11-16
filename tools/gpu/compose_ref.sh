#!/bin/sh
# The compose pass's CPU reference (tools/gpu/compose_ref.c): the shader before the savings (OLD, a
# git revision; 5abd258 by default) and as it is now, compiled for the host and compared.
# Usage: tools/gpu/compose_ref.sh [OLD]   (from anywhere in the repository)
# SPDX-License-Identifier: GPL-3.0-or-later
set -e
OLD=${1:-5abd258}
ROOT=$(git rev-parse --show-toplevel)
CC=${CC:-/opt/homebrew/opt/llvm/bin/clang}
D=$(mktemp -d)
trap 'rm -rf "$D"' EXIT
# (the T#s' vector type aligned to 32 as on the GPU: a host may align vectors to 16 at the most)
A='s/ext_vector_type(8)))/ext_vector_type(8), aligned(32)))/'
git -C "$ROOT" show "$OLD:tools/gpu/compose.c" | sed "$A" > "$D/old.c"
sed "$A" "$ROOT/tools/gpu/compose.c" > "$D/new.c"
# (the GPU's intrinsics become the reference's functions; the constants' address space dropped)
HOST="-O2 -ffp-contract=off -Wno-deprecated-builtins -D__asm(x)= -Daddress_space(x)="
$CC $HOST -Dcompose_main=compose_old -c -o "$D/old.o" "$D/old.c"
$CC $HOST -Dcompose_main=compose_new -c -o "$D/new.o" "$D/new.c"
$CC -O2 -ffp-contract=off -o "$D/ref" "$ROOT/tools/gpu/compose_ref.c" "$D/old.o" "$D/new.o" -lm
"$D/ref"
