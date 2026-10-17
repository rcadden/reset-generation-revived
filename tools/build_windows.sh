#!/bin/bash
# Builds dist/ResetGeneration-Revived: a self-contained Windows app (32-bit Java runtime + game).
# Needs: the original game files in game/ (checked in) and a 32-bit JDK 17.
set -e
R="$(cd "$(dirname "$0")/.." && pwd)"
JDK=${JDK:-/c/dev/toolchains/jdk-17.0.17+10}
B=$R/build/win
OUT=$R/dist
APP=ResetGeneration-Revived
w() { cygpath -w "$1"; }

rm -rf "$B" "$OUT/$APP"
mkdir -p "$B/classes" "$B/jar" "$B/input" "$OUT"

# 1. Pristine game files (game/ = the 2020 standalone without its old JRE).
G="$R/game"

# 2. Compile the Revived launcher + patched classes against the original jars.
(cd "$R/src/java" && "$JDK/bin/javac" -nowarn --release 17 -encoding UTF-8 \
  -cp "$(w $G/ResetGeneration.jar);$(w $G/lib/rg-signed.jar)" \
  -d "$(w $B/classes)" $(find . -name '*.java'))

# 3. Original jar contents, overlaid with our classes, plus the icon.
(cd "$B/jar" && unzip -q -o "$G/ResetGeneration.jar" && rm -rf META-INF)
cp -r "$B/classes/." "$B/jar/"
mkdir -p "$B/jar/revived" && cp "$R/src/assets/icon.png" "$B/jar/revived/icon.png"
printf 'Main-Class: rg.Revived\nClass-Path: rg-signed.jar\n' > "$B/manifest.txt"
"$JDK/bin/jar" --create --file "$(w $B/input/ResetGenerationRevived.jar)" --manifest "$(w $B/manifest.txt)" -C "$(w $B/jar)" .
cp "$G/lib/rg-signed.jar" "$B/input/"

# 4. Self-contained app image: ResetGeneration.exe + trimmed 32-bit runtime.
"$JDK/bin/jpackage" --type app-image --name "$APP" --dest "$(w $OUT)" \
  --input "$(w $B/input)" --main-jar ResetGenerationRevived.jar --main-class rg.Revived \
  --icon "$(w $R/src/assets/icon.ico)" --app-version 1.0.0 \
  --vendor "Reset Generation Revived" --description "Reset Generation (RedLynx/Nokia, 2008) for modern Windows" \
  --add-modules java.base,java.desktop,java.net.http,jdk.jsobject \
  --jlink-options "--strip-debug --no-header-files --no-man-pages --compress=2" \
  --java-options "-Dsun.java2d.uiScale=1" --java-options "-Xss1m"

# 5. Game data next to the jar (the engine looks in <jar dir>/ResetGeneration).
D="$OUT/$APP/app/ResetGeneration"
mkdir -p "$D"
for f in "$G"/ResetGeneration/*; do
  case "$(basename "$f")" in
    *.dylib|file.lock) ;;            # macOS build / stale lock file
    *) cp "$f" "$D/" ;;
  esac
done
cp "$R/src/dist/revived.ini" "$OUT/$APP/app/revived.ini"
cp "$R/src/dist/README.txt" "$OUT/$APP/README.txt"
echo "Built $OUT/$APP"
du -sh "$OUT/$APP"
