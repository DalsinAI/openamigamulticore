#!/bin/sh
# Copyright (c) 2026 Dalsin Limited. OpenMulticore, MIT licence (LICENSE).
# SPDX-License-Identifier: MIT
# openmulticore.library (bare: no startup code or C library; its RomTag's
# stub comes first) and OMCTest, with the os32 stove (bebbo's m68k-amigaos-gcc).
#   library/build.sh [OUT_DIR]     (default build/)
# openservice.device's headers come from a checkout of openamigaservice
# beside this one (OPENSERVICE=dir to give another).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
STOVE=${STOVE:-$HOME/AmigaChrome/stoves/os32}
CC=${CC:-$STOVE/prefix/bin/m68k-amigaos-gcc}
OS=${OPENSERVICE:-$ROOT/../openamigaservice}
OUT=${1:-$ROOT/build}
mkdir -p "$OUT"
"$CC" -m68020 -O2 -fno-delete-null-pointer-checks -fomit-frame-pointer -fno-toplevel-reorder -fno-builtin -Wall -Wextra -Werror -Wno-unused-parameter \
    -nostartfiles -nostdlib -I"$ROOT/include" -I"$OS/include" \
    -o "$OUT/openmulticore.library" "$HERE/openmulticore_lib.c" -lgcc
echo "$OUT/openmulticore.library ($(wc -c < "$OUT/openmulticore.library") bytes)"
"$CC" -m68020 -O2 -fno-delete-null-pointer-checks -Wall -Werror -Wno-pointer-sign -noixemul -I"$ROOT/include" -o "$OUT/OMCTest" "$ROOT/tools/omctest.c"
echo "$OUT/OMCTest ($(wc -c < "$OUT/OMCTest") bytes)"
