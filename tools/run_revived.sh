#!/bin/bash
# run_revived.sh [args...] -- run Revived launcher from build/classes against the standalone game dir
R="$(cd "$(dirname "$0")/.." && pwd)"; S=$R/extracted/standalone/ResetGeneration-Standalone
$R/tools/killgame.sh; sleep 1
cd $S
CP="$(cygpath -w $R/build/classes);ResetGeneration.jar;lib/rg-signed.jar"
(/c/dev/toolchains/jdk-17.0.17+10/bin/java.exe -cp "$CP" rg.Revived "$@" > "$TEMP/rgexp.log" 2>&1 &)
