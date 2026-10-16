#!/bin/bash
# k.sh "KEYS" name  -- send keys to game, screenshot to scratchpad/name.png
T="$(cd "$(dirname "$0")" && pwd)"
SP="${SP:-${TEMP:-/tmp}/rg-shots}"; mkdir -p "$SP"
powershell -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w $T/rgkeys.ps1)" -Keys "$1" -Shot "$SP/$2.png" >/dev/null 2>&1
tasklist | grep -q java || echo "GAME NOT RUNNING"
[ -n "$3" ] && py -I -c "from PIL import Image; import sys; Image.open(sys.argv[1]).resize((int(sys.argv[2]),int(sys.argv[2])*9//16)).save(sys.argv[1])" "$SP/$2.png" $3
true
