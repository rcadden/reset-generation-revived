#!/bin/bash
# setup_dev.sh -- create the writable dev copy of the game that run_revived.sh / k.sh / the emulator
# use (extracted/standalone/ResetGeneration-Standalone), from the checked-in game/ files.
R="$(cd "$(dirname "$0")/.." && pwd)"; S=$R/extracted/standalone/ResetGeneration-Standalone
if [ -d "$S/ResetGeneration" ]; then echo "exists: $S"; exit 0; fi
mkdir -p "$S" && cp -r "$R/game/." "$S/" && echo "created $S"
