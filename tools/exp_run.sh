#!/bin/bash
# exp_run.sh W H [extra java args] -- run patched wrapper at a given native res
R="$(cd "$(dirname "$0")/.." && pwd)"; S=$R/extracted/standalone/ResetGeneration-Standalone
$R/tools/killgame.sh; sleep 1
cd $S
CP="$(cygpath -w $R/build/exp);ResetGeneration.jar;lib/rg-signed.jar"
W=$1; H=$2; shift 2
(/c/dev/toolchains/jdk-17.0.17+10/bin/java.exe -Drg.w=$W -Drg.h=$H "$@" -cp "$CP" rg.StandaloneWrapper > "$TEMP/rgexp.log" 2>&1 &)
