#!/bin/bash
# Kill only the game's java process (not Ghidra or other JVMs).
powershell -NoProfile -Command "Get-CimInstance Win32_Process -Filter \"Name='java.exe'\" | Where-Object { \$_.CommandLine -match 'rg\.(Revived|StandaloneWrapper)|ResetGeneration\.jar' } | ForEach-Object { Stop-Process -Id \$_.ProcessId -Force }" >/dev/null 2>&1
