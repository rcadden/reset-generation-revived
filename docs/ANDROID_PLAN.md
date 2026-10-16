# Android port plan

Goal: Reset Generation as a real Android game (Play-distributable APK/AAB, landscape, touch
controls, gamepad support), single-player first, multiplayer later.

## The constraint

We have no engine source. The engine exists as:

* `ResetGeneration-Win32-Web.dll` — **x86 (32-bit) Windows**, ~1.5 MB of code, clean, and
  readable in Ghidra (decompilation exported locally to `decompiled/native/rg_win32.c`, not in the repo);
* `ResetGeneration.exe` from the N-Gage SIS — ARMv5 Symbian; its data is DRM-encrypted and
  modern Android phones are increasingly 64-bit-only (no AArch32), so it is not a viable base.

So Android needs one of: run the x86 engine on ARM, translate it to ARM ahead of time, or rewrite it.

## What we proved (Phase 0, done)

`tools/emu/rgemu.py` loads the **unmodified x86 DLL into a CPU emulator (Unicorn) with ~100 Win32
functions and ~15 JNI functions implemented by hand**, then drives it exactly like the Java
wrapper does. Results:

* The engine boots, mounts its paks, plays the intro, **accepts input (ESC → main menu)**,
  renders correct frames at 640×360, and produces audio. No Windows, no Java.
* The platform surface really is small: `kernel32` file I/O, heap, TLS, critical sections,
  `timeGetTime`/`Sleep`; JNI string/buffer calls; one direct pixel buffer; one audio pull callback.
* **Performance:** about 12 million guest instructions per frame; native x86 needs ~5 ms per
  frame. Unicorn ran at ~100 ms/frame on this desktop (≈120 MIPS) — **too slow for 32 fps**,
  and a phone would be slower. Hot spots: `rep movsd` blits (QEMU executes REP one iteration per
  dispatch) and a 16-bit RLE sprite decoder.
* **The engine uses no floating point** in its hot paths (fixed-point, built for the FPU-less
  N-Gage). This removes the hardest part of translating x86 code.
* The engine paces itself (32 fps) purely through `timeGetTime()`/`Sleep()`, which the host
  supplies — so the host fully controls time (useful for pause/resume and for deterministic tests).

## Options

| Option | How | Verdict |
|---|---|---|
| A. Interpreter/JIT emulation (Unicorn/QEMU TCG) | Ship the DLL + emulator + shim | Works today, **~8 fps**. Good as a test oracle, not as the product. |
| B. Fast dynarec (Box64 "box32", FEX) | Reuse a high-performance x86→ARM64 JIT | Fast enough, but these are Linux-ELF process emulators, not embeddable libraries for a Windows DLL. High integration risk; possible fallback. |
| **C. Static recompilation** | Translate every function of the DLL ahead of time into portable C (from Ghidra P-code or a capstone lifter) against a flat 32-bit guest memory, then compile with the NDK for arm64-v8a/x86_64 | **Recommended.** Near-native speed, no JIT (Play-safe), debuggable C, and the original engine's exact behaviour (AI, rules, timing). The engine is an ideal candidate: MSVC code, no self-modifying code, no FPU, no threads, tiny import surface. |
| D. Full rewrite (Godot/Kotlin/C++) | Re-implement rules, AI, UI using decompilation + original assets | Best long-term flexibility (true HD UI, new content) but months of work and behaviour drift. Keep as a later option, not the port. |
| E. Wine + Box64 container (Winlator-style) | Run the Windows build on Android | Heavy, poor touch integration, not a "real" Android game. Rejected. |

## Recommended plan

### Phase 1 — native host on Windows (2–4 days)

A small C program (`rghost`) with **SDL2** that loads the 32-bit DLL directly (no Java) and
implements the JNI calls the engine makes (a fake `JNIEnv` table — the PoC shows exactly which
ones), the framebuffer upload (SDL texture, integer scaling), audio (pull `onSoundBufferEmpty`,
resample 8 kHz → device rate), keyboard + **gamepad**, the virtual clock, and the pak patching
(port of `PakPatcher`). This becomes the **platform layer** shared with Android, and also gives
a lighter Java-free Windows build with gamepad support.
*Toolchain:* portable llvm-mingw (i686 target) — no Visual Studio needed.

### Phase 2 — static recompiler (2–4 weeks; the main risk)

1. Export from the existing Ghidra project: function list, P-code per function, switch tables,
   vtables and other code pointers in data.
2. Generate C: one C function per guest function; registers in a `cpu` struct; memory through a
   `uint8_t *mem` base (guest addresses stay 32-bit); flags computed lazily; indirect calls and
   returns through an address → function table. `rep movs/stos` become `memcpy`/`memset`.
3. Replace recognised MSVC CRT routines (malloc/free/memcpy/str*/file I/O, identified by Ghidra
   function-ID signatures) with native implementations; route the remaining imports to the
   Phase 1 Win32 shim.
4. **Validate against the oracle**: run the Unicorn build and the recompiled build with the same
   scripted inputs and virtual clock, and compare framebuffer hashes every frame (intro, menus,
   tutorial, story missions, 4-AI arcade matches). Any divergence pinpoints the bad function.
5. Ship the recompiled engine on Windows x64 first (replaces the 32-bit JVM dependency entirely).

Fallback if Phase 2 stalls: evaluate embedding Box64's dynarec behind the Phase 1 shim.

### Phase 3 — Android app (≈1 week)

* NDK + SDL2 (its Android Java glue gives the Activity, surface, audio, gamepads, lifecycle),
  `arm64-v8a` + `x86_64`, min SDK 24, target current. **Native libs must be 16 KB-page aligned**
  (Play requirement for new apps targeting Android 15+).
* Game data (~32 MB of paks) packed as APK assets and copied to internal storage on first run
  (the engine expects ordinary file I/O; alternatively implement the file shim on
  `AAssetManager`). Paks pre-patched at build time for the chosen resolution.
* Landscape only. Engine resolution chosen from the screen aspect with ≥320 rows
  (e.g. 640×360 for 16:9, 720×360 / 800×360 for 18:9–20:9 phones), integer- or
  smooth-scaled to the display; cutout/safe-area aware.
* Lifecycle: on pause stop ticking and audio and **freeze the virtual clock** (otherwise the
  frame limiter tries to "catch up" after resume); saves (`settings.cfg`, `achievements.dat`)
  in app-private storage with Android Auto Backup.

### Phase 4 — touch controls

The game needs only **4 directions + 1 Action button** (tap = rotate/select, hold = drop/confirm)
plus Select/Back — touch-friendly by design.

* **MVP: on-screen controls.** Translucent d-pad (left thumb) and a large Action button (right
  thumb) with native tap/hold semantics, small Select/Back buttons in the top corners, haptic
  feedback, auto-hide when a gamepad is connected, size/opacity/position settings. The playfield
  stays unobstructed because the widescreen layout leaves the side areas free — the controls
  live where the original screen never drew.
* **Enhanced: direct touch.** Tap a grid square to move the block/cursor there, tap again (or
  long-press) to act; swipe to rotate; tap menu items. Implemented by reading the engine's
  cursor/playfield state from guest memory (available once recompiled) and synthesising the
  arrow presses. Ship after MVP.
* Gamepads and keyboards (Chromebooks) work through SDL out of the box.

### Phase 5 — multiplayer (optional, 1–2 weeks)

Implement the engine's SNAP-Mobile interface in the host and connect it to our own room-relay
server, giving Windows ↔ Android cross-play. See `MULTIPLAYER.md`.

### Phase 6 — release

Play listing under Applied Flux; content rating questionnaire (cartoon violence, suggestive
character art → likely Teen); privacy policy (no data collected for offline play); internal →
closed → production tracks as with Drywater.

## Risks

| Risk | Mitigation |
|---|---|
| Recompiler correctness (indirect jumps, flag edge cases, MSVC EH frames) | Per-frame oracle comparison against the emulator; fall back to the interpreter for single functions while debugging |
| C++ exceptions/SEH actually used at runtime | PoC saw none during boot/intro/menus; implement `RaiseException` + frame-chain unwinding only if the oracle hits one |
| Engine assumes 32-bit pointers | Guest memory stays a flat 32-bit space; host pointers never enter guest memory |
| Performance on low-end phones | Native C; 640×360 software rendering is light (the original ran on ~330–370 MHz ARM11 N-Gage 2.0 phones at 240×320) |
| Licensing of bundled fonts/libs | Larabie fonts permit commercial bundling; SDL2 is zlib; keep notices |

## Immediate next actions

1. Install portable llvm-mingw + SDL2 dev libraries; build `rghost` (Phase 1) using
   `tools/emu/rgemu.py` as the reference for every shim function.
2. Extend the PoC into the **oracle**: scripted input files, framebuffer hash logging per frame.
3. Prototype the recompiler on a handful of leaf functions (the RLE sprite decoder at RVA
   `0x9df80` is a good first target: hot, pure integer, self-contained).
