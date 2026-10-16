# Reset Generation — game and engine analysis

Findings from unpacking both downloads, decompiling the Java wrapper, decompiling the
native engine with Ghidra, and running the game. Written 2026-10-16.

## 1. What the two downloads are

| File | What it is | Usable? |
|------|------------|---------|
| `ResetGeneration-Standalone.zip` (64 MB) | The **2009 web (Java applet) version** of Reset Generation, revived in 2020 as a standalone app by curoviyxru, bodyz and bs0dd (Russian readme). Java wrapper + native x86 engine DLL + plaintext `.pak` data + bundled 32-bit JRE 6. | **Yes — this is our base.** |
| `Reset.Generation.n-gage` (27 MB) | An **N-Gage 2.0 install file**: MIME multipart (`RetailerInfo` XML, `Metadata` SIS, `Game` SIS). The Game SIS (Symbian 9, UID `0x2000AFC4`, v1.0.21) contains an ARM `ResetGeneration.exe`, a `ResetGenCrack.dll` (it is a cracked copy), achievement icons/XML, Arena config, and the same nine `.pak` files. | **Data is not usable**: every `.pak` is wrapped in OMA DRM (`odcf`/`odrm` container), i.e. encrypted. The ARM binary is a Symbian executable. Useful only as reference (achievements list, Arena config). |

The web version and the N-Gage version share the engine and data layout; the web version's
paks are a superset (it adds `fonts.pak` with Chinese/Russian fonts) and are not encrypted.

Tools written for this: `tools/split_ngage.py` (MIME split), `tools/sisx.py` (SIS v9 extractor),
`tools/pak.py` (pak extractor with real file names).

## 2. Architecture of the Windows (web) build

```
start.bat → java -jar ResetGeneration.jar           (rg.StandaloneWrapper, a JFrame)
              └─ rg.LauncherApplet (java.applet.Applet inside the frame)
                   └─ rg.ResetGeneration (main loop, input queue, Java2D blit)
                        ├─ System.load(ResetGeneration/*.dll)
                        │     ResetGeneration-Win32-Web.dll  ← the actual game (C++, x86, MSVC 2005-era)
                        │     zlib1.dll
                        ├─ NativeGameEngine.init(IntBuffer fb, lang, mode, width, height, …)
                        ├─ notifyDataPackageReady("base.pak" … "menus.pak")
                        └─ loop: tick() → blit fb → Graphics.drawImage (scaled)
                   SoundOutput thread: onSoundBufferEmpty(ByteBuffer,1024) → javax.sound (8 kHz mono 16-bit)
                   SNAPMobileClient (lib/rg-signed.jar): Nokia SNAP Mobile 1.4.4 online client
```

* **All game logic, rendering, audio mixing and UI live in the native DLL.** Java only owns
  the window, keyboard, the audio device and the network socket.
* The DLL is self-contained: it imports ~90 `kernel32` functions plus `winmm!timeGetTime`, no
  graphics or audio APIs, and creates no threads. It renders in software into a 32-bit
  `0x00RRGGBB` buffer that Java hands it as a direct `IntBuffer` (`width × (height+1)` ints).
* JNI functions the engine calls back into: `GetStringUTFChars`/`Release…`, `NewStringUTF`,
  `GetDirectBufferAddress`, `GetObjectClass`, `NewGlobalRef`, `GetMethodID`, `CallVoidMethodV`
  (plus the same set from the SNAP glue). This tiny surface is what makes a Java-free host —
  and the Android plan — practical.
* Exported entry points (all `__stdcall` JNI): `NativeGameEngine.{init, deinit, tick,
  setHomeDirectory, notifyDataPackageReady, getLanguage, isInWaitScreen, getSelectButton,
  getBackButton, setVolume, setTime/getTime/getTotalTime, setReplayData/playReplay/pauseReplay/
  setReplayMode, isReplayVisualizationInProgress}`, `NativeInputEventListener.{onKeyPressed,
  onKeyReleased, onKeyTyped, releaseAllControls}`, `NativeSoundOutputDevice.{onSoundBufferEmpty,
  onSoundInitFailed}`, and the `NativeSNAPMobile*` networking glue.
* Input is Java `KeyEvent` virtual-key codes passed straight through; the engine has its own
  key-binding table (Settings → Custom Keys).
* **Timing:** the engine self-paces at 32 fps (`1000/32 ms`). Each `tick()` runs a frame
  limiter on `timeGetTime()`: when ahead of schedule it calls the app's draw routine and then
  `Sleep()`s the remainder. Rendering therefore only happens when the engine has spare time.
* **Arithmetic is fixed-point.** A profile of the running engine showed **no x87 or SSE
  instructions** in the hot paths (the code was built for the FPU-less N-Gage ARM9). The hot
  loops are `rep movsd` blits and a 16-bit RLE sprite decoder.
* **Screen size is a parameter.** The original web build passes 240×320 (portrait), but the
  engine lays its UI out for any size we pass — this is the key fact behind the HD port (see
  `WINDOWS_PORT.md`).

Engine internals worth knowing (addresses are RVAs in `ResetGeneration-Win32-Web.dll`, image base
`0x10000000`):

| What | Where |
|------|-------|
| `NativeGameEngine.init` | `0x2860` — builds the global app object, package manager, frame timer (32 fps) |
| `tick` | `0x2c00` — app update, package manager update, net update, frame limiter, returns `isRunning` |
| Package manager | `0x3540`–`0x3740` — maps pak names to 9 slots; mounts one ready pak per tick |
| "Wait for package" state | `0x43ff0` ctor, `0x440d0` enter (sets `isInWaitScreen`), `0x441c0` check/draw |
| App draw | `0x83d0` — `display.begin() → stateStack.draw(gc) → display.end() → present` |
| Frame callback | `0x1070` — calls app draw with elapsed time, then idle/sleep with the remainder |
| Wipe-transition blit | `0x793a0` — composites new screen through a mask texture (crashed above 240×320; fixed by resizing masks) |
| Pak file name hash | djb2-style (`h = h*33 + c`, seed 5381) at `0xefda0`; the exact path normalisation is still unidentified (not needed so far) |

## 3. Data files (`ResetGeneration/*.pak`)

**Container format** (little endian):

```
u32 header_size        (= 8 + 17*count)
u32 count
count × { u32 name_hash; u32 stored_size; u32 raw_size; u8 zlib; u32 offset }
data…                   zlib streams when the flag is 1
```

The **last entry of every pak is its file-name list** (`u32 n; n × {u16 len; char[len]}`), in
entry order, so every file can be extracted with its original path
(`py -I tools/pak.py extract <pak> <outdir>`).

| Pak | Entries | Contents |
|-----|--------:|----------|
| `base.pak` | 48 | Fonts (TrueType + 4 bitmap fonts), all 8 language string tables (`.lng`), palettes, wipe-transition masks, loading music, sound bank, "portrait mode" image |
| `fonts.pak` | 4 | Chinese (Simplified/Traditional) and Russian TrueType fonts |
| `intro.pak` | 117 | Attract-mode intro: 19 scene scripts (`STORY_intro_part*.xml`), art, music |
| `story.pak` | 444 | Story-mode cutscenes: scene scripts, animated portraits, voice-over |
| `menus.pak` | 433 | Menu backgrounds/animations, menu music, UI sounds |
| `gameplay.pak` | 1917 | Playfields, blocks, heroes (sprite sheets + animation XML), units, effects, items, AI, triggers, SFX/VO |
| `gameplay2.pak` | 15 | Extra mission trigger scripts |
| `fight.pak` | 335 | The close-up fight animations (attack cut-ins) |
| `levels.pak` | 8 | Mission definitions (`story/missions.xml`) and the six multiplayer maps |

Inner formats:

| Ext | Count | Format |
|-----|------:|--------|
| `.xml` | 802 | Plain XML: missions/maps (grid data as CSV rows), scene scripts (`<effect name="SPRITE|TILESCROLLER|PARTICLE|SUB|TEXT">` with `<physics type="RESET" positionX/Y …>`), hero animation tables, AI, triggers. Highly moddable. |
| `.gfx` | 780 | 40-byte header (`u32 2, u32 format, u32 w, u32 h, u32 frames, …, u32 data_size@36`). Format 0 = raw 32-bit ARGB; format 4 = 16-bit RLE (`count, 0x8000|palette-index` pairs); format 2 = multi-frame sheets with an offset table (fonts, sprites). Indexed images use external `.pal` palettes, which is how team colours work. `tools/gfx.py` decodes format 0. |
| `.spc` | 1669 | Audio clips: music, SFX and voice-over (format not yet decoded) |
| `.lng` | 8 | String tables, `KEY value` per line (UTF-16 for zh/ru, 8-bit for others). Exported to `extracted/strings/`. |
| `.ttf` | 5 | Fonts (Larabie "free for commercial use" fonts per the N-Gage `read_me.html`) |
| `.pal`, `.ass`, `.ndl`, `.dir` | | Palettes, sound bank, misc |

## 4. The game

**Premise.** A comedy love letter to 8- and 16-bit games: heroes who parody classic game
characters (a muscled red Plumber, a blue Hedgehog, a Level 50 Elf, a Ninja, a Sci-Fi Knight, a
Cyborg, a Babe Gunner, a Monster Trainer, Dr. Lovebomber and the Aggressor) fight to rescue
Princesses. Developed by RedLynx for Nokia; released 2008 on N-Gage and free on the web in 2009.

**Rules (from the in-game tutorial text and play).**

* Up to **4 players** (human or AI) on a grid playfield (from 6×15 strips to 12×10 and 11×11 arenas) with a Castle
  and a Princess per player. **Win** by carrying an enemy Princess back to your Castle; if an
  enemy carries yours home, you lose.
* Every round has **simultaneous, timed phases** — everybody acts at once and the results resolve
  together:
  1. **Blocks** — drop a Tetris-style piece onto empty squares (arrows to move, tap Action to
     rotate, hold Action to drop). Pieces that collide on the same square cancel out.
     **Five starred blocks in a line form a Combo**: you move farther and fight better on your
     own Combos and are weaker on others'. Combos also charge the *other* players' power meters.
  2. **Cannons** — shoot two enemy blocks to destroy them (Combos and heroes are immune; firing
     can also protect items).
  3. **Move** — draw a movement path with the arrows and confirm; ending on an enemy attacks it
     (knocking him out makes him drop the Princess he carries), ending on a Princess or item
     picks it up.
* **Items**: Grenade, Teleport, Laser Cannon, Combo Builder, Invisibility, Spring, Egg, Wand
  ("frog" a hero), Battery (fills the power meter), Monster Box.
* **Powers** (one per hero, used when the power meter is full): Plumber *Ladies Man* (grab almost
  any item on the map), Hedgehog *Hog Ride*, Cyborg *Rocket Jump*, Level 50 Elf *Combo Stick +1*,
  Babe Gunner *Bullet Ballet*, Monster Trainer *Evil Dark Wizard*, Sci-Fi Knight *Sci-Fi Mind
  Trick*, Ninja *Ninja Vanish*, Dr. Lovebomber *Lovebomb*, Aggressor *4-Bit Weapon*.
* Neutral **monsters** roam some maps (pig, wolf, chicken/zombie chicken/egg, Blockapede, foe,
  wizard, mega monster); heroes can be turned into a Frog.

**Modes.** Single-Player → Tutorial (3 lessons), Story Mode (9 chapters × 2 battles = 18 missions,
one per hero with comic cutscenes and voice-over), Arcade Mode (pick hero and up to 3 AI
opponents); Quick Match; Online (N-Gage Arena — dead since 2010); Settings (Sound, Language,
Custom Keys, Use Default, Erase Data); Info. 8 languages: English, French, German, Italian,
Spanish, Russian, Chinese Simplified and Traditional.

**Controls.** The whole game is playable with **four directions plus one action button**
(tap = rotate/select, hold = drop/confirm) plus Select/Back soft keys. This is the single most
important fact for the Android port: it maps directly onto a touch d-pad + one button.

**Saves.** `ResetGeneration/settings.cfg` (binary, `SYEK` key-binding block plus settings) and
`achievements.dat`, written next to the paks.

## 5. Online and replays (summary)

Online used Nokia **SNAP Mobile** (gateway `resetgeneration-gw.arena.n-gage.com`, lobby `resgen`,
game class 42713). The web build also had a **replay player mode** (420×378 window with
play/pause/seek controls, replay data downloaded from a URL). Details and the plan for our own
server are in `MULTIPLAYER.md`.
