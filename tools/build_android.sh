#!/bin/bash
# Builds the Android APK (sideloadable, debug-signed) into dist/:
#   recompiled engine (tools/recomp) + native/rt + native/android host -> libresetgen.so
#   game data from game/ (pre-patched for the 640x360 engine size) as APK assets.
# No Gradle or Java code: NativeActivity + aapt2/zipalign/apksigner from the SDK.
# Options: ABIS="arm64-v8a x86_64"  VERSION=0.1.0  SERVER=ws://host:8787/ws (default relay)
set -e
R="$(cd "$(dirname "$0")/.." && pwd)"
SDK=${ANDROID_SDK:-/c/dev/toolchains/android-sdk}
NDK=${NDK:-$SDK/ndk/29.0.14206865}
BT=$SDK/build-tools/36.0.0
PLATFORM=$SDK/platforms/android-36/android.jar
JDK=${JDK17:-/c/dev/toolchains/jdk-17.0.17+10}
OUT=${RG_BUILD:-/c/dev/rgbuild}
ABIS=${ABIS:-"arm64-v8a x86_64"}
VERSION=${VERSION:-0.1.0}
VERSION_CODE=${VERSION_CODE:-1}
SERVER=${SERVER:-ws://127.0.0.1:8787/ws}
CLANG="$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe"
GEN=$OUT/gen
w() { cygpath -m "$1"; }
mkdir -p "$OUT" "$R/dist"

# 1. generated C
if [ ! -f "$GEN/rg_gen_table.c" ] || [ "$R/tools/recomp/recomp.py" -nt "$GEN/rg_gen_table.c" ] || [ "$R/decompiled/native/pcode.txt" -nt "$GEN/rg_gen_table.c" ]; then
  [ -f "$R/decompiled/native/pcode.txt" ] || { echo "missing decompiled/native/pcode.txt: run tools/ghidra_export.sh"; exit 1; }
  rm -rf "$GEN"
  py -I "$R/tools/recomp/recomp.py" "$R/decompiled/native/pcode.txt" "$GEN" --dll "$R/game/ResetGeneration/ResetGeneration-Win32-Web.dll"
fi

# 2. native library per ABI
STAMP="$VERSION-$(date +%s)"
for ABI in $ABIS; do
  case $ABI in
    arm64-v8a) TRIPLE=aarch64-linux-android26 ;;
    x86_64) TRIPLE=x86_64-linux-android26 ;;
    *) echo "unknown ABI $ABI"; exit 1 ;;
  esac
  OBJ=$OUT/obj-$ABI
  mkdir -p "$OBJ"
  CF="--target=$TRIPLE -O2 -fPIC -fno-strict-aliasing -fwrapv -ffunction-sections -fdata-sections -I$(w $R/native/rt) -I$(w $GEN) -I$(w $NDK/sources/android/native_app_glue)"
  echo "== $ABI: compiling generated code"
  ls "$GEN"/*.c | xargs -P 8 -I{} sh -c 'o="'"$OBJ"'/$(basename {} .c).o"; [ "$o" -nt "{}" ] || "'"$CLANG"'" '"$CF"' -w -c "$(cygpath -w {})" -o "$(cygpath -w $o)"'
  for f in "$R"/native/rt/*.c "$R"/native/android/main.c "$NDK/sources/android/native_app_glue/android_native_app_glue.c"; do
    "$CLANG" $CF -Wall -Wno-unused-function -DRG_DEFAULT_SERVER="\"$SERVER\"" -DRG_DATA_STAMP="\"$STAMP\"" \
      -c "$(w $f)" -o "$(w $OBJ/$(basename "$f" .c).o)"
  done
  mkdir -p "$OUT/apk/lib/$ABI"
  "$CLANG" --target=$TRIPLE -shared -o "$(w $OUT/apk/lib/$ABI/libresetgen.so)" $(for o in "$OBJ"/*.o; do w "$o"; done) \
    -u ANativeActivity_onCreate -Wl,--gc-sections -Wl,-z,max-page-size=16384 -Wl,--build-id=sha1 -s \
    -landroid -llog -laaudio -lm
  ls -la "$OUT/apk/lib/$ABI/libresetgen.so"
done

# 3. game data (assets), patched for the 640x360 engine size
A=$OUT/assets/game
rm -rf "$OUT/assets" && mkdir -p "$A"
cp "$R"/game/ResetGeneration/* "$A/"
rm -f "$A"/*.dylib "$A"/zlib1.dll
CLS=$OUT/patchtool
mkdir -p "$CLS"
"$JDK/bin/javac" -nowarn --release 17 -cp "$(w $R/game/ResetGeneration.jar)" -d "$(w $CLS)" \
  "$(w $R/src/java/rg/PatchTool.java)" "$(w $R/src/java/rg/PakPatcher.java)" "$(w $R/src/java/rg/DllPatcher.java)"
"$JDK/bin/java" -cp "$(w $CLS);$(w $R/game/ResetGeneration.jar)" rg.PatchTool "$(w $A)" 640 360
# the recompiled engine is built from the original DLL (its data and imports are used at run time)
mv -f "$A/ResetGeneration-Win32-Web.dll.orig" "$A/ResetGeneration-Win32-Web.dll"
rm -f "$A"/*.orig "$A"/*.revived

# 4. package
M=$OUT/AndroidManifest.xml
sed "s/@VERSION_CODE@/$VERSION_CODE/; s/@VERSION_NAME@/$VERSION/" "$R/native/android/AndroidManifest.xml" > "$M"
rm -rf "$OUT/res-compiled" && mkdir -p "$OUT/res-compiled"
"$BT/aapt2.exe" compile --dir "$(w $R/native/android/res)" -o "$(w $OUT/res-compiled/res.zip)"
"$BT/aapt2.exe" link -o "$(w $OUT/unsigned.apk)" -I "$(w $PLATFORM)" --manifest "$(w $M)" \
  -A "$(w $OUT/assets)" "$(w $OUT/res-compiled/res.zip)"
py -I - "$OUT/unsigned.apk" "$OUT/apk" <<'PY'
import os, sys, zipfile
apk, root = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(apk, 'a', zipfile.ZIP_DEFLATED) as z:
    for d, _, files in os.walk(os.path.join(root, 'lib')):
        for f in files:
            p = os.path.join(d, f)
            z.write(p, os.path.relpath(p, root).replace(os.sep, '/'))
PY
"$BT/zipalign.exe" -f -p 4 "$(w $OUT/unsigned.apk)" "$(w $OUT/aligned.apk)"
KS=$OUT/debug.keystore
if [ ! -f "$KS" ]; then
  "$JDK/bin/keytool" -genkeypair -keystore "$(w $KS)" -storepass android -keypass android -alias rgdebug \
    -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Reset Generation Revived debug" >/dev/null 2>&1
fi
APK="$R/dist/ResetGeneration-Revived-$VERSION.apk"
JAVA_HOME="$(w $JDK)" "$BT/apksigner.bat" sign --ks "$(w $KS)" --ks-pass pass:android --ks-key-alias rgdebug \
  --out "$(w $APK)" "$(w $OUT/aligned.apk)"
JAVA_HOME="$(w $JDK)" "$BT/apksigner.bat" verify "$(w $APK)" && ls -la "$APK"
