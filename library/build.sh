#!/bin/sh
# Copyright (c) 2026 AmigaChrome Team. OpenMulticore, MIT licence (LICENSE).
# SPDX-License-Identifier: MIT
# openmulticore.library (bare: no startup code or C library; its RomTag's
# stub comes first), OMCTest and OMCBench, with the os32 stove (bebbo's m68k-amigaos-gcc).
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
# The 68040, soft float: the specs file links libnix's soft libm, not libm881.
SOFT="-m68040 -specs=$ROOT/specs/m68k-soft-m68040.specs"
# OpenAmigaGCC 16.2.0-open3's late-combine pass can store a copy one place late (GCC PR 123853): run_local's copy of
# a job's arguments onto its stack came out "move.l (a0)+,(a0,d3.l)", each argument a longword higher (why OpenUp
# builds this with the os32 stove: "it passes jobs the wrong arguments", 8 October 2026). Off until OpenAmigaGCC
# open4 (openamigagcc #29), on a compiler that knows the option (the os32 stove's GCC 6.5 doesn't). Every compile
# here takes $SOFT, so it goes there.
if "$CC" -fno-late-combine-instructions -S -o /dev/null -x c /dev/null 2>/dev/null; then SOFT="$SOFT -fno-late-combine-instructions"; fi
mkdir -p "$OUT"
"$CC" $SOFT -O2 -fno-delete-null-pointer-checks -fomit-frame-pointer -fno-toplevel-reorder -fno-builtin -Wall -Wextra -Werror -Wno-unused-parameter \
    -nostartfiles -nostdlib -I"$ROOT/include" -I"$OS/include" \
    -o "$OUT/openmulticore.library" "$HERE/openmulticore_lib.c" -lgcc
echo "$OUT/openmulticore.library ($(wc -c < "$OUT/openmulticore.library") bytes)"
"$CC" $SOFT -O2 -fno-delete-null-pointer-checks -Wall -Werror -Wno-pointer-sign -noixemul -I"$ROOT/include" -o "$OUT/OMCTest" "$ROOT/tools/omctest.c"
echo "$OUT/OMCTest ($(wc -c < "$OUT/OMCTest") bytes)"
"$CC" $SOFT -O2 -fno-delete-null-pointer-checks -Wall -Werror -Wno-pointer-sign -noixemul -I"$ROOT/include" -I"$OS/include" -o "$OUT/OMCBench" "$ROOT/tools/omcbench.c"
echo "$OUT/OMCBench ($(wc -c < "$OUT/OMCBench") bytes)"
