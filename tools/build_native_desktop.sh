#!/bin/bash
# Builds $RG_BUILD/rgtest.exe (default C:\dev\rgbuild, outside OneDrive): the recompiled engine
# + desktop test host (Windows x64, llvm-mingw). Regenerates the C from
# decompiled/native/pcode.txt when the generator or the export is newer.
set -e
R="$(cd "$(dirname "$0")/.." && pwd)"
CC=${CC:-/c/dev/toolchains/llvm-mingw-20261006-ucrt-x86_64/bin/x86_64-w64-mingw32-clang}
OUT=${RG_BUILD:-/c/dev/rgbuild}
GEN=$OUT/gen
OBJ=$OUT/obj-desktop
mkdir -p "$OBJ"
if [ ! -f "$GEN/rg_gen_table.c" ] || [ "$R/tools/recomp/recomp.py" -nt "$GEN/rg_gen_table.c" ] || [ "$R/decompiled/native/pcode.txt" -nt "$GEN/rg_gen_table.c" ]; then
  rm -rf "$GEN"
  py -I "$R/tools/recomp/recomp.py" "$R/decompiled/native/pcode.txt" "$GEN" --dll "$R/game/ResetGeneration/ResetGeneration-Win32-Web.dll"
fi
CFLAGS="-O2 ${EXTRA_CFLAGS:-} -fno-strict-aliasing -fwrapv -I$R/native/rt -I$GEN"
ls "$GEN"/*.c | xargs -P 8 -I{} sh -c 'o="'"$OBJ"'/$(basename {} .c).o"; [ "$o" -nt "{}" ] || '"$CC $CFLAGS"' -w -c "{}" -o "$o"'
for f in "$R"/native/rt/*.c "$R"/native/host/desktop/rgtest.c; do
  $CC $CFLAGS -g -Wall -Wno-unused-function -c "$f" -o "$OBJ/$(basename "$f" .c).o"
done
$CC -o "$OUT/rgtest.exe" "$OBJ"/*.o -lws2_32 -lpthread -lm -static
echo "built $OUT/rgtest.exe"
