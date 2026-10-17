# Reset Generation Revived

Bringing **Reset Generation** (RedLynx / Nokia, 2008 — N-Gage and free web release) to modern
Windows in widescreen HD, to Android, and back online with our own server.

| | Status |
|---|---|
| Windows 10/11, widescreen HD, fullscreen | **Playable** — `tools/build_windows.sh` → `dist/ResetGeneration-Revived/ResetGeneration-Revived.exe` |
| Android (phones, tablets, Chromebooks; touch + gamepad) | **Playable test build** — `tools/build_android.sh` → `dist/ResetGeneration-Revived-<version>.apk` (sideload) |
| Multiplayer server | **Partly working** — login, casino and Quick Match matchmaking work; the match itself does not start yet |

## Start here

* `docs/GAME_ANALYSIS.md` — what the downloads are, how the game and engine work, file formats, mechanics
* `docs/WINDOWS_PORT.md` — what the Windows build changes, how to build it, known issues
* `docs/NATIVE_PORT.md` — the static recompiler, the native runtime, the Android app and how to build them
* `docs/ANDROID_PLAN.md` — why static recompilation (options, emulation proof of concept)
* `docs/MULTIPLAYER.md` — the original online stack, our relay and casino server, the reconstructed protocol

## Layout

```
game/                            the original game files (engine DLL, jars, data paks), included with permission
src/java/rg/                     Windows launcher + patched wrapper classes; rg.PatchTool for build-time data patches
src/java/com/redlynx/...         replacement online client (talks to our relay instead of N-Gage Arena)
src/assets/, src/dist/           icon, default revived.ini, player README
native/rt/                       runtime for the recompiled engine: memory, Win32 + JNI shims, engine API, net client
native/android/                  Android NativeActivity host (touch controls, gamepad, audio), manifest, icons
native/host/desktop/             rgtest: headless desktop test driver for the recompiled engine
server/                          multiplayer relay + casino matchmaker (Node server and Cloudflare Worker)
tools/                           build scripts, recompiler (tools/recomp), Ghidra export, extractors, test drivers
tools/emu/                       the engine under Unicorn emulation (oracle for the recompiler)
decompiled/                      CFR / Ghidra output and the P-code export (generated locally; git-ignored)
extracted/, build/, dist/        unpacked data, build outputs (git-ignored)
Reset.Generation.n-gage          original N-Gage 2.0 package (reference only, DRM-encrypted; git-ignored)
```

## Build

```bash
tools/build_windows.sh            # Windows: 32-bit JDK 17 (see docs/WINDOWS_PORT.md)
tools/ghidra_export.sh            # once: Ghidra analysis + P-code export of the engine DLL
tools/build_android.sh            # Android APK: Android SDK/NDK (see docs/NATIVE_PORT.md)
cd server && npm install && npm start    # multiplayer relay on ws://localhost:8787/ws
```

Toolchains are portable installs under `C:\dev\toolchains` (Temurin 17 x86 and 21 x64,
Ghidra 12, llvm-mingw, Android SDK with NDK 29); native build outputs go to `C:\dev\rgbuild`
(outside OneDrive).

## Game files and decompiled code

`game/` holds the original game files, included with permission: the 2020 standalone of the
web version (`ResetGeneration.jar`, `lib/rg-signed.jar`, `ResetGeneration/` with the engine
DLL, `zlib1.dll`, the macOS engine and all data paks), minus its obsolete Java 6 runtime. They
are kept pristine; every fix is applied at build or launch time.

Decompiled code is not included: `decompiled/` and the recompiler's generated C are produced
locally (CFR on the jars, `tools/ghidra_export.sh` on the DLL). The one exception is
`src/java/rg/ResetGeneration.java`, a modified decompile of the original wrapper class, kept
because the launcher needs to replace it to fix the screen size and scaling.

## Credits

Reset Generation © 2008 RedLynx / Nokia. 2020 standalone revival by curoviyxru, bodyz and
bs0dd. Fonts by Larabie Fonts.
