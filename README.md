# Reset Generation Revived

Bringing **Reset Generation** (RedLynx / Nokia, 2008 — N-Gage and free web release) to modern
Windows in widescreen HD, with a plan for Android and for our own multiplayer server.

| | Status |
|---|---|
| Windows 10/11, widescreen HD, fullscreen | **Playable** — `dist/ResetGeneration-Revived/ResetGeneration-Revived.exe` |
| Android | Plan written; core risk de-risked by an emulation proof of concept |
| Multiplayer server | Feasible; protocol and architecture documented |

## Start here

* `docs/GAME_ANALYSIS.md` — what the downloads are, how the game and engine work, file formats, mechanics
* `docs/WINDOWS_PORT.md` — what the Windows build changes, how to build it, known issues
* `docs/ANDROID_PLAN.md` — options, proof-of-concept results, recommended phased plan
* `docs/MULTIPLAYER.md` — the original SNAP Mobile online stack and how to replace it

## Layout

```
ResetGeneration-Standalone.zip   original 2020 standalone of the web version (input; git-ignored)
Reset.Generation.n-gage          original N-Gage 2.0 package (reference only; git-ignored)
src/java/rg/                     Revived launcher + patched wrapper classes (Java 8 bytecode)
src/assets/, src/dist/           icon, default revived.ini, player README
tools/                           build script, extractors (SIS, pak, gfx), test drivers, Ghidra export
tools/emu/                       x86-engine-under-emulation proof of concept (Android groundwork)
decompiled/                      CFR output of both jars; Ghidra C export of the engine DLL (generated; git-ignored)
extracted/                       unpacked downloads, paks, string tables (generated; git-ignored)
build/, dist/                    build outputs (git-ignored)
```

## Build the Windows release

```bash
tools/build_windows.sh
```

Needs a portable 32-bit Temurin JDK 17 at `C:\dev\toolchains\jdk-17.0.17+10` (see
`docs/WINDOWS_PORT.md`).

## What this repository does not contain

No game binaries, data paks or decompiled third-party code are distributed here. To build or
study the game you need your own copy of the original standalone (`ResetGeneration-Standalone.zip`).
`decompiled/` is regenerated locally: run CFR on `ResetGeneration.jar` and `lib/rg-signed.jar`, and
`tools/ghidra_scripts/ExportDecomp.java` on the engine DLL. The one exception is
`src/java/rg/ResetGeneration.java`, a modified decompile of the original wrapper class, kept
because the launcher needs to replace it to fix the screen size and scaling.

## Credits

Reset Generation © 2008 RedLynx / Nokia. 2020 standalone revival by curoviyxru, bodyz and
bs0dd. Fonts by Larabie Fonts.
