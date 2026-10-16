#!/bin/bash
# to_battle.sh W H shotprefix [java args] : boot at WxH, navigate to Story > Battle 1-1, screenshot along the way
T="$(cd "$(dirname "$0")" && pwd)"
SP="${SP:-${TEMP:-/tmp}/rg-shots}"; mkdir -p "$SP"
W=$1; H=$2; P=$3; shift 3
if [ "$W" = auto ]; then $T/run_revived.sh "$@"; else $T/run_revived.sh --render_width=$W --render_height=$H "$@"; fi; sleep 9
k() { local shot=(); [ -n "$2" ] && shot=(-Shot "$SP/$P$2.png"); powershell -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w $T/rgkeys.ps1)" -Keys "$1" "${shot[@]}" 2>&1 | grep -v "^title" | head -2; tasklist | grep -q java || { echo "DEAD at $2"; grep -o 'Win32-Web.dll+0x[0-9a-f]*' $TEMP/rgexp.log; exit 1; }; }
k "RETURN,WAIT2000,RETURN,WAIT1500,DOWN,WAIT500,RETURN,WAIT2500" _a
k "RETURN,WAIT3000" _b
for i in 1 2 3 4 5 6 7 8 9 10 11 12; do k "RETURN,WAIT900"; done
k "WAIT1500" _c
for i in 1 2 3 4 5 6; do k "RETURN,WAIT900"; done
k "WAIT1500" _d
echo ALIVE
