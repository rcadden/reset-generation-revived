/* Platform-independent API to the recompiled Reset Generation engine. Single-threaded:
 * call everything from one thread. */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Boots the engine. game_dir holds the DLL's data (*.pak, settings.cfg, ...) and must be
 * writable; dll_path is the original ResetGeneration-Win32-Web.dll (only its data sections
 * and import table are used; the code is the recompiled C). */
int rg_init(const char *dll_path, const char *game_dir, int width, int height, const char *lang);
/* One engine step (the engine paces itself to 32 fps through timeGetTime/Sleep).
 * Returns 0 when the game asked to quit. */
int rg_tick(void);
/* Framebuffer: width*height pixels, 0x00RRGGBB. */
const uint32_t *rg_pixels(void);
/* Java AWT key codes: 37..40 arrows, 10 Enter, 27 Escape, 17 Ctrl, 32 Space. */
void rg_key(int down, int keycode);
/* Pulls engine audio: 8 kHz mono signed 16-bit. len in bytes (even). */
void rg_audio(uint8_t *buf, int len);
void rg_set_volume(int volume);   /* 0..100 */
int rg_in_wait_screen(void);
/* Deterministic clock for tests: time advances only through Sleep() and rg_advance_ms(). */
void rg_use_virtual_clock(int on);
void rg_advance_ms(uint32_t ms);

/* ------------------------------------------------------------------ online play
 * The engine's SNAP Mobile interface is served by a network client (net.c, relay protocol in
 * docs/MULTIPLAYER.md). The host sets the server and nickname before rg_init. */
void rg_net_configure(const char *server_url, const char *nickname);

#ifdef __cplusplus
}
#endif
