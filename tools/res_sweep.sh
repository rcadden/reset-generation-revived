#!/bin/bash
# Try several native resolutions; report alive/crash and save screenshots.
T="$(cd "$(dirname "$0")" && pwd)"
SP="$1"; shift
for res in "$@"; do
  W=${res%x*}; H=${res#*x}
  $T/exp_run.sh $W $H; sleep 13
  if tasklist | grep -q java; then
    powershell -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w $T/winshot.ps1)" -Proc java -Out "$(cygpath -w $SP)\res_$res.png"
    echo "$res ALIVE"
  else
    echo "$res CRASH: $(grep -o 'Win32-Web.dll+0x[0-9a-f]*' $TEMP/rgexp.log)"
  fi
done
$T/killgame.sh
rm -f $T/../extracted/standalone/ResetGeneration-Standalone/hs_err_pid*.log
