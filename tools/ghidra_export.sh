#!/bin/bash
# Imports + analyses the engine DLL in a headless Ghidra project (first run only) and exports
# decompiled/native/pcode.txt (for tools/recomp) and decompiled/native/rg_win32.c (for reading).
# Env: GHIDRA (install dir), JAVA21 (JDK 21+), GHIDRA_PROJ (project dir).
set -e
R="$(cd "$(dirname "$0")/.." && pwd)"
GHIDRA=${GHIDRA:-/c/dev/toolchains/ghidra_12.1.4_PUBLIC}
JAVA21=${JAVA21:-/c/dev/toolchains/jdk-21.0.12.1+1}
PROJ=${GHIDRA_PROJ:-/c/dev/ghidra_proj}
DLL=${1:-$R/game/ResetGeneration/ResetGeneration-Win32-Web.dll}
w() { cygpath -w "$1"; }
mkdir -p "$PROJ" "$R/decompiled/native"
if [ -d "$PROJ/RG.rep" ]; then MODE="-process $(basename "$DLL") -noanalysis"; else MODE="-import $(w "$DLL")"; fi
BAT="$PROJ/export_run.bat"
cat > "$BAT" <<BATEOF
@echo off
set JAVA_HOME=$(w "$JAVA21")
call "$(w "$GHIDRA")\support\analyzeHeadless.bat" "$(w "$PROJ")" RG $MODE -scriptPath "$(w "$R/tools/ghidra_scripts")" -postScript ExportPcode.java "$(w "$R/decompiled/native/pcode.txt")" -postScript ExportDecomp.java "$(w "$R/decompiled/native/rg_win32.c")"
BATEOF
powershell -NoProfile -Command "& '$(w "$BAT")'" > "$PROJ/export.log" 2>&1 || true
grep -q "ExportPcode: wrote" "$PROJ/export.log" || { echo "Ghidra export failed, see $PROJ/export.log"; exit 1; }
ls -la "$R/decompiled/native/"
