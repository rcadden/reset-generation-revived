# Windows 11 HD port ("Reset Generation Revived")

Status (2026-10-16): **playable**. Self-contained build at `dist/ResetGeneration-Revived/`
(76 MB, no Java install needed). Verified on Windows 11 at 1920×1080: intro, menus, settings,
tutorial, story mode (cutscenes and battles), arcade 4-player matches, fullscreen toggle,
audio device, in-game Quit.

## What changed vs. the 2020 standalone

| Problem in the 2020 build | Fix |
|---|---|
| Fixed 240×320 portrait screen, integer zoom only | The engine is initialised at a **widescreen virtual resolution** (default 640×360) and integer-scaled to the window. The engine re-lays-out its own UI, so menus, HUD and the playfield use the extra space — a whole 12×12 arena fits on screen. |
| Ancient bundled JRE 6 | Bundled **Temurin 17 (32-bit)** runtime, trimmed with `jlink` (the engine DLL is 32-bit, so the JVM must be too). |
| No fullscreen | **Borderless fullscreen**: F11 or Alt+Enter, remembered in `revived.ini`. |
| Keyboard ignored until you clicked the window; a second click showed a missing help overlay | Focus is requested automatically; the dead help-overlay click is disabled. |
| Above 240×320 the game crashed on the first screen transition (and showed striped wipes) | The wipe masks in `base.pak` are 240×320 and the blit loops over the whole screen; the launcher now **regenerates the masks at the engine resolution** (originals kept as `base.pak.orig`). |
| Above 240×320, screens left stale pixels where the original art did not reach | The engine framebuffer is **cleared before every frame**. |
| Cutscenes drawn in the top-left corner and cut off at the bottom | Cutscene scripts in `intro.pak`/`story.pak` use absolute 240×320 coordinates; the launcher **offsets them to the centre** of the screen. |
| Attack cut-ins ("fight" popups) drawn against the left edge | Their frame and 240×320 surface are drawn at hard-coded positions; `DllPatcher` **patches those two draw positions** in a copy of the engine DLL (original kept as `.dll.orig`, patch sites verified byte-for-byte first). |
| Launched via `start.bat` | `ResetGeneration-Revived.exe` with an icon. |

Apart from that one verified position patch, the native engine is unmodified: the fixes are in
Java and in data.

## How it works

* `src/java/rg/Revived.java` — new entry point (replaces `rg.StandaloneWrapper`). Reads
  `app/revived.ini` (command-line `--key=value` overrides), picks the engine resolution, owns the
  window and fullscreen toggling.
* `src/java/rg/ResetGeneration.java` — the decompiled original with small changes: configurable
  screen size, per-frame native clear, aspect-correct integer scaling with black bars
  (`present()`), help-overlay click removed.
* `src/java/rg/SharedBackBuffer.java` — adds `clearNative()`.
* `src/java/rg/PakPatcher.java` — rebuilds `base.pak` (wipe masks), `intro.pak` and
  `story.pak` (scene offsets) from `*.pak.orig` copies whenever the resolution changes; a
  `*.pak.revived` stamp avoids redoing it on every launch. Paks an older version patched but the
  current one does not are restored from `.orig`.
* `src/java/rg/DllPatcher.java` — centres the fight popup (see the class comment for the exact
  patch: two immediates plus a 21-byte code cave in the unused tail of `.text`).
* Everything else is the original `ResetGeneration.jar` / `rg-signed.jar`, repacked.

**Resolution choice.** `render_height=auto` takes the largest integer scale that keeps at least
320 rows (the cutscenes are 320 tall): 640×360 on any 16:9 monitor, shown at ×2 (720p), ×3
(1080p), ×4 (1440p) or ×6 (4K). Lower values (e.g. `render_height=270` → 480×270) make
everything bigger. Tested engine sizes: 240×320 up to 1280×720; 1280×720 works but sprites become
tiny and cutscene art cannot fill it.

## Build

Requirements: the game files in `game/` (checked in), a 32-bit JDK 17
(`C:\dev\toolchains\jdk-17.0.17+10`, portable Temurin x86 zip; override with `JDK=`), Git Bash.

```bash
tools/build_windows.sh
```

Output: `dist/ResetGeneration-Revived/` — `ResetGeneration-Revived.exe`, `runtime/`, `app/`
(jars, `revived.ini`, `ResetGeneration/` game data), `README.txt`. Zip the folder to distribute.

Developer loop without packaging: `tools/setup_dev.sh` once, then `tools/run_revived.sh
[--render_height=…]` runs the compiled classes against `extracted/standalone/`; `tools/k.sh "KEYS" name` sends keys to the window and
takes a screenshot (`tools/rgkeys.ps1`, `tools/winshot.ps1`).

## Known issues / polish backlog

1. Some **menu backgrounds** are narrower than 640 px (e.g. the opponent-select space backdrop),
   leaving black on the right. Menus are laid out correctly; it is art coverage only. Fix idea:
   extend `PakPatcher` to centre or tile `menus.pak` background sprites.
2. Cutscene backdrops are 240 px wide: centred with black (or tiled effect layers) either side.
   The end-of-match **win/lose screens** render into a fixed 240×320 surface at the left (their
   scripts cannot simply be shifted — tried and reverted); centring them needs a position patch
   like the fight popup's once its draw call is located.
   A nicer frame (blurred/extended art) would need new art.
3. "**Quick Match**" and "**Online**" now log in to our relay (`server/`, set `server=` in
   `revived.ini`; no Arena login screen). Quick Match pairs players into a game room, but the
   match itself does not start yet (`MULTIPLAYER.md`, "Next step").
4. The game folder must be **writable** (saves and the patched paks live in it): don't install
   under `Program Files`.
5. No **gamepad** support (Java has none built in). Options: a small XInput→key bridge in the
   launcher via JNA, or the native host planned for Android (SDL2 gamepads come free).
6. Audio is the engine's original 8 kHz mono mix — faithful to the original, but low fidelity.
7. Windows "Smart App Control"/SmartScreen may warn about the unsigned exe on other PCs.

## Next step for Windows (optional)

Replacing Java entirely with a small native host (C + SDL2) that loads the engine DLL directly
and implements the ~10 JNI calls it uses — the same host the Android plan needs. Benefits: tiny
download, gamepads, better audio resampling, no JVM. See `ANDROID_PLAN.md` phase 1.
