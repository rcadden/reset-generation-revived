# Native port: recompiled engine, desktop test host, Android APK

The Android version (and a future Java-free Windows build) runs the original x86 engine as
**portable C produced by static recompilation** — option C of `ANDROID_PLAN.md`. No emulator
and no JIT: every instruction of `ResetGeneration-Win32-Web.dll` is translated ahead of time.

```
game/ResetGeneration/ResetGeneration-Win32-Web.dll
   │  Ghidra (headless): tools/ghidra_scripts/ExportPcode.java
   ▼
decompiled/native/pcode.txt        raw P-code of all 468k instructions (local, git-ignored)
   │  tools/recomp/recomp.py
   ▼
$RG_BUILD/gen/rg_gen_*.c           ~12,000 C functions + address→function table (generated)
   │  + native/rt/      runtime: guest memory, dispatch, Win32 + JNI shims, engine glue, net client
   │  + native/host/desktop/rgtest.c   headless test driver (Windows, llvm-mingw)
   │  + native/android/main.c          NativeActivity host (NDK)
   ▼
rgtest.exe  /  libresetgen.so → dist/ResetGeneration-Revived-<ver>.apk
```

## How the translation works

* **Input is Ghidra P-code**, not x86 decoded by hand: Ghidra's SLEIGH spec gives exact
  semantics for every instruction, including flags. `ExportPcode.java` writes one line per
  instruction and P-code op, plus Ghidra's function entries and switch-table targets.
* **One C function per guest function.** Entries are Ghidra's functions, plus code addresses
  found in data (vtables: 1,271 extra) and in instruction immediates (callbacks pushed as
  arguments). A function's body is everything reachable from its entry without crossing a
  call; code shared between functions is duplicated; a jump into another entry is a tail call.
* **Guest memory** is one flat 4 GB reservation; guest pointers stay 32-bit (`M + addr`).
  Memory map: stack `0x00100000`, TEB `0x00400000`, JNI tables `0x00500000`, image
  `0x10000000`, heap `0x20000000`, VirtualAlloc `0x40000000`, host stubs `0xF0000000+`.
* **Registers:** EAX..EDI are copied into C locals at function entry and spilled around calls,
  so the C compiler keeps them in host registers. Flags are function-local (MSVC code never
  keeps flags live across a call) and dead flag computations disappear in the optimiser.
  x87, DF and rare registers live in a register file indexed by Ghidra's register offsets;
  80-bit x87 values are host doubles (the engine's hot code is integer/fixed-point).
* **Calls** push the real return address on the guest stack (code that inspects it still
  works) and call the C function; `RET` pops it and returns. Indirect calls and jumps go
  through `rt_call()` (hash table), which also routes the fake addresses of imports and JNI
  functions to host stubs.
* **Fast paths:** `REP MOVS/STOS` become `memmove`/`memset`; `CPUID` reports a Pentium
  without SSE2, so the CRT never takes its SSE2 paths (the only instructions left untranslated).

## Runtime (native/rt)

| File | Role |
|---|---|
| `rt.h`, `rt.c` | memory access helpers used by generated code, dispatch, string ops, CPUID, 80-bit floats |
| `win32.c` | PE loader (sections, exports, IAT → stubs), guest heap, the 86 kernel32/winmm imports |
| `jni.c` | fake `JNIEnv` (240-entry table in guest memory): strings, direct buffers, `Call*Method` → host |
| `engine.c`, `engine.h` | boot sequence and per-frame API (`rg_init`, `rg_tick`, `rg_key`, `rg_audio`, `rg_pixels`) |
| `net.c`, `net.h` | online play: the engine's SNAP Mobile interface on the relay (`MULTIPLAYER.md`), ws:// |

The Win32/JNI behaviour mirrors `tools/emu/rgemu.py`, the Unicorn emulator used as the oracle.

## Building

Prerequisites (portable, under `C:\dev\toolchains`): Ghidra 12 + JDK 21 (export), llvm-mingw
(desktop), Android SDK with NDK 29, build-tools 36 and platform 36 (APK), 32-bit JDK 17
(build-time data patching). Build outputs go to `$RG_BUILD` (default `C:\dev\rgbuild`) —
keep them out of OneDrive: the generated code is ~90 MB of C.

```bash
tools/ghidra_export.sh            # once per DLL change: Ghidra analysis -> decompiled/native/pcode.txt
tools/build_native_desktop.sh     # -> $RG_BUILD/rgtest.exe (headless test driver)
tools/build_android.sh            # -> dist/ResetGeneration-Revived-<version>.apk
```

`build_android.sh` options: `ABIS="arm64-v8a x86_64"` (default both), `VERSION=0.1.0`,
`SERVER=ws://host:8787/ws` (default relay address baked into the APK).

## Android app

Install the debug APK by sideloading: copy `dist/ResetGeneration-Revived-<version>.apk` to the
phone and open it (allow "install unknown apps" for the file manager), or
`adb install -r dist/ResetGeneration-Revived-<version>.apk`.

* NativeActivity with no Java code; min SDK 26 (Android 8), target 36; arm64-v8a + x86_64,
  16 KB page-aligned native library.
* Game data ships as assets and is copied to internal storage on first launch of each build
  (saves are kept). The paks are pre-patched for the 640×360 engine size (`rg.PatchTool`).
* The window buffer is 720 px high at the screen's aspect ratio; the game is drawn 2× in the
  middle and the touch controls sit in the side bars: d-pad (left), Action (right, tap/hold
  like Ctrl), Select and Back (top corners). Tapping the engine's "Select"/"Back" soft-key
  labels at the bottom corners works too. Gamepads and keyboards: d-pad/stick, A = Action,
  Start/X = Select, B/Back = Back; on-screen controls hide while a pad is used.
* Audio: AAudio, the engine's 8 kHz mono mix resampled to the device rate.
* Pause/resume freezes the engine clock so its 32 fps limiter does not race on resume.
* Online: `files/revived.ini` in the app's internal storage holds `server=` and `nickname=`
  (written on first launch with the build's default server and a random name).

## Testing

`rgtest.exe <dll> <data dir> <frames> <out dir> [--keys F:KEY,...] [--every N] [--realtime]`
runs the engine (virtual clock unless `--realtime`), writes `hashes.txt` (framebuffer hash per
frame) and PPM screenshots. `RG_NICK` / `RG_SERVER` set the online identity.

Verified (2026-10-17): boot, intro, menus, wipes, tutorial with input on the desktop driver;
the APK on an Android 15 x86_64 emulator (intro, menu, touch d-pad/Select into a mode);
Quick Match through login, casino and matchmaking against the local relay.

Differential testing against the original: `RT_API_TRACE=file rgtest …` and
`py -I tools/emu/rgemu.py <dir> <out> <frames> 640 360 --api-trace file` log every Win32/JNI
call; the first divergence points at the bug (155,659 calls matched after the fixes).

Debug build (`EXTRA_CFLAGS=-DRT_CALLTRACE tools/build_native_desktop.sh`): crashes print the
last 48 function entries and the guest call stack; `RECOMP_ARGS="--trace a,b"` logs entries of
chosen functions; `RT_LIVE_TRIGGER=addr:N` logs the N function entries after `addr`;
`RT_NET_DEBUG=1` logs incoming packets.

## Known limits

* On 20:9 phones the side bars are narrow, so the d-pad and Action button overlap the edges of
  the game image (translucent). A layout option (smaller game, wider bars) would help.
* Debug-signed APK (`C:\dev\rgbuild\debug.keystore`); a release key is needed for Play.

* The fight-popup position patch (`DllPatcher`) is not in the recompiled build yet; it needs
  the P-code export to be taken from the patched DLL.
* C++ exceptions / SEH unwinding are not implemented (`RaiseException` aborts); none were seen.
* The native net client speaks plain `ws://` only (no TLS yet).
