# Reset Generation Revived — project notes for agents

Read `README.md`, then the doc for the area you touch (`docs/`). Work on `dev`; `main` only on
explicit instruction. Public repo: github.com/rcadden/reset-generation-revived. Never commit personal paths/emails or
decompiled code (`decompiled/` is git-ignored). The original game files in `game/` are included
with permission; keep them pristine (patch at build/launch time, never in place).

## Facts that are easy to get wrong

* The real game is the **native x86 DLL** `ResetGeneration-Win32-Web.dll`; Java is only a host.
  The DLL is 32-bit, so any JVM that loads it must be **32-bit** (we use Temurin 17 x86 at
  `C:\dev\toolchains\jdk-17.0.17+10`; JDK 21 x64 there is only for Ghidra).
* The engine accepts **any screen size** at `init` and lays out its UI for it. Default is
  640×360 (≥320 rows so 240×320 cutscenes fit).
* The **N-Gage paks are DRM-encrypted**; only the web/standalone paks are usable.
* Game data paks are patched at launch by `PakPatcher` from `*.pak.orig` (wipe masks, cutscene
  offsets). Never edit `*.pak` by hand; change the transform and bump `PakPatcher.VERSION`.
* The engine only draws when ahead of its 32 fps schedule (frame limiter on `timeGetTime`);
  hosts must supply time and let `Sleep()` advance it.
* No floating point in the engine's hot code (fixed-point).
* The engine's clock must start small: it misbehaves with large `timeGetTime` values (long
  uptimes), so hosts rebase it (`rt_now_ms` starts at 5,000,000).
* Android/native = **static recompilation**: Ghidra P-code (`tools/ghidra_export.sh`) →
  `tools/recomp/recomp.py` → C. Code only reachable through vtables/stored callbacks must be
  recovered by the export's pre-pass; an "unknown address" fatal means a missed entry point.
* Online: the engine waits for the room's server to speak first; `CasinoSystem` is served by
  the casino bot in `server/src/casino.js`. The in-room protocol (seating, match) is not done.

## Tools

* `tools/build_windows.sh` — full release build into `dist/`.
* `tools/build_android.sh` — APK into `dist/` (NDK + aapt2, no Gradle). `tools/build_native_desktop.sh`
  — `C:\dev\rgbuild\rgtest.exe`, headless test driver (`EXTRA_CFLAGS=-DRT_CALLTRACE`,
  `RECOMP_ARGS="--trace addr,…"` for debugging). Native build output stays in `C:\dev\rgbuild`
  (never in OneDrive).
* `server/` — relay + casino: `npm start`, `npm test`, `--log-packets`.
* `tools/setup_dev.sh` then `tools/run_revived.sh [--render_height=N …]` — run compiled classes against `extracted/standalone/`.
* `tools/k.sh "KEYS" name [width]` — send keys (`RETURN,ESC,UP,DOWN,LEFT,RIGHT,CTRL,F11,WAIT500`)
  to the game window and screenshot to the session scratchpad. `tools/killgame.sh` kills only the
  game JVM (never `taskkill java.exe` — Ghidra is also a JVM).
* `tools/pak.py`, `tools/gfx.py`, `tools/sisx.py`, `tools/split_ngage.py` — data extraction.
* `tools/emu/rgemu.py` — engine under Unicorn emulation (oracle for the Android recompiler).
* Ghidra 12.1.4 at `C:\dev\toolchains\ghidra_12.1.4_PUBLIC`, project `C:\dev\ghidra_proj\RG`.
* Android SDK at `C:\dev\toolchains\android-sdk` (NDK 29, build-tools 36, emulator; AVD `rgtest` in
  `C:\dev\avd`, WHPX-accelerated); llvm-mingw at `C:\dev\toolchains\llvm-mingw-20261006-ucrt-x86_64`.

## Python

Run scripts with `py -I`. Unpacked third-party data lives under `extracted/` — never run code from there.
