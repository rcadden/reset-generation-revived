/* Android host for the recompiled Reset Generation engine (NativeActivity, no Java code).
 *
 * - Game data ships as APK assets and is copied to internal storage on first run (the engine
 *   does ordinary file I/O and writes its saves next to the data).
 * - The engine draws 640x360 (data pre-patched for that size at build time). The window buffer
 *   is 2x that height at the screen's aspect ratio; the game is scaled 2x into the middle and
 *   the touch controls live in the side bars, so they never cover the playfield.
 * - Input: on-screen d-pad + Action/Select/Back, hardware keyboards and gamepads, Back key.
 * - Audio: AAudio stream fed from the engine's 8 kHz mono mixer.
 * - Pausing (home, screen off) freezes the engine clock so its frame limiter does not try to
 *   catch up on resume. */
#include <aaudio/AAudio.h>
#include <android/asset_manager.h>
#include <android/log.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <android/window.h>
#include <android_native_app_glue.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "engine.h"
#include "host.h"

#define TAG "ResetGeneration"
#define GW 640
#define GH 360
#define SCALE 2
#define BH (GH * SCALE)

/* Java AWT key codes the engine understands */
enum { K_ENTER = 10, K_CTRL = 17, K_ESC = 27, K_LEFT = 37, K_UP = 38, K_RIGHT = 39, K_DOWN = 40 };

/* ------------------------------------------------------------------ host services */
void host_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    __android_log_vprint(ANDROID_LOG_INFO, TAG, fmt, ap);
    va_end(ap);
}

void host_fatal(const char *msg) {
    __android_log_print(ANDROID_LOG_FATAL, TAG, "fatal: %s", msg);
    abort();
}

static uint64_t mono_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

static uint64_t paused_total, paused_at;
static int paused = 1;
uint32_t host_time_ms(void) { return (uint32_t)(mono_ms() - paused_total); }
void host_sleep_ms(uint32_t ms) { if (ms) usleep(ms * 1000); }

static void set_paused(int p) {
    if (p == paused) return;
    if (p) paused_at = mono_ms();
    else paused_total += mono_ms() - paused_at;
    paused = p;
}

/* ------------------------------------------------------------------ assets -> internal storage */
/* Copies the APK's assets/game/ files once per APK build (stamp file). Existing saves
 * (settings.cfg, achievements.dat, *.save) are never overwritten. */
static int install_data(AAssetManager *am, const char *dir, const char *stamp_want) {
    char path[512], have[64] = "";
    mkdir(dir, 0755);
    snprintf(path, sizeof path, "%s/.installed", dir);
    FILE *f = fopen(path, "r");
    if (f) { if (!fgets(have, sizeof have, f)) have[0] = 0; fclose(f); }
    if (!strcmp(have, stamp_want)) return 1;
    AAssetDir *ad = AAssetManager_openDir(am, "game");
    const char *name;
    int n = 0;
    static char buf[65536];
    while ((name = AAssetDir_getNextFileName(ad))) {
        char asset[256];
        struct stat st;
        snprintf(asset, sizeof asset, "game/%s", name);
        snprintf(path, sizeof path, "%s/%s", dir, name);
        int is_save = !strcmp(name, "settings.cfg") || !strcmp(name, "achievements.dat") || strstr(name, ".save");
        if (is_save && stat(path, &st) == 0) continue;
        AAsset *a = AAssetManager_open(am, asset, AASSET_MODE_STREAMING);
        if (!a) continue;
        FILE *o = fopen(path, "wb");
        if (!o) { AAsset_close(a); continue; }
        int k;
        while ((k = AAsset_read(a, buf, sizeof buf)) > 0) fwrite(buf, 1, (size_t)k, o);
        fclose(o);
        AAsset_close(a);
        n++;
    }
    AAssetDir_close(ad);
    host_log("installed %d data files into %s", n, dir);
    snprintf(path, sizeof path, "%s/.installed", dir);
    f = fopen(path, "w");
    if (f) { fputs(stamp_want, f); fclose(f); }
    return n > 0;
}

/* ------------------------------------------------------------------ audio */
#define RING 16384                      /* samples (2 s at 8 kHz) */
static int16_t ring[RING];
static volatile uint32_t ring_r, ring_w;
static AAudioStream *stream;
static int out_rate = 48000;
static double resample_pos;

static aaudio_data_callback_result_t audio_cb(AAudioStream *s, void *user, void *data, int32_t frames) {
    (void)s; (void)user;
    int16_t *out = (int16_t *)data;
    double step = 8000.0 / out_rate;
    for (int32_t i = 0; i < frames; i++) {
        uint32_t avail = ring_w - ring_r;
        int16_t v = 0;
        if (avail > 1) {
            int16_t a = ring[ring_r % RING], b = ring[(ring_r + 1) % RING];
            v = (int16_t)(a + (b - a) * resample_pos);       /* linear interpolation */
            resample_pos += step;
            while (resample_pos >= 1.0) { resample_pos -= 1.0; ring_r++; }
        }
        out[i] = v;
    }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void audio_start(void) {
    AAudioStreamBuilder *b;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return;
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, 1);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setDataCallback(b, audio_cb, NULL);
    if (AAudioStreamBuilder_openStream(b, &stream) == AAUDIO_OK) {
        out_rate = AAudioStream_getSampleRate(stream);
        AAudioStream_requestStart(stream);
        host_log("audio: %d Hz", out_rate);
    }
    AAudioStreamBuilder_delete(b);
}

static void audio_stop(void) {
    if (stream) { AAudioStream_requestStop(stream); AAudioStream_close(stream); stream = NULL; }
}

/* keep ~100 ms of engine audio queued */
static void audio_pump(void) {
    static uint8_t buf[2048];
    while (ring_w - ring_r < 800) {
        rg_audio(buf, 512);
        for (int i = 0; i < 256; i++) { ring[ring_w % RING] = ((int16_t *)buf)[i]; ring_w++; }
    }
}

/* ------------------------------------------------------------------ touch controls */
typedef struct { int key; float cx, cy, r; const char *label; int down; } button;
static button buttons[] = {
    {K_UP, 0, 0, 0, "up", 0}, {K_DOWN, 0, 0, 0, "down", 0}, {K_LEFT, 0, 0, 0, "left", 0}, {K_RIGHT, 0, 0, 0, "right", 0},
    {K_CTRL, 0, 0, 0, "action", 0}, {K_ENTER, 0, 0, 0, "select", 0}, {K_ESC, 0, 0, 0, "back", 0},
    {K_CTRL, 0, 0, 0, "drop", 0},
};
#define B_DROP 7
/* The game drops a piece when Action is held for 500 ms. The Drop button is a tap that holds
 * Ctrl for DROP_MS by itself, so no finger has to stay down. */
#define DROP_MS 700
static int drop_pending;
static uint64_t drop_release_at;
#define NB (int)(sizeof buttons / sizeof buttons[0])
static int bw;                           /* window buffer width (height is BH) */
static int game_x;                       /* left edge of the game image in the buffer */
static int controls_visible = 1;
static uint64_t last_pad_input;
static int pointer_button[16];           /* which button each touch pointer holds (-1 none) */

static void layout_controls(void) {
    float side = (float)game_x;               /* width of each side bar */
    float u = BH / 10.0f;
    float lx = side > 3 * u ? side / 2 : 2.2f * u, ly = BH * 0.62f;
    float d = 1.25f * u, r = 0.85f * u;
    buttons[0] = (button){K_UP, lx, ly - d, r, "up", 0};
    buttons[1] = (button){K_DOWN, lx, ly + d, r, "down", 0};
    buttons[2] = (button){K_LEFT, lx - d, ly, r, "left", 0};
    buttons[3] = (button){K_RIGHT, lx + d, ly, r, "right", 0};
    float rx = bw - (side > 3 * u ? side / 2 : 2.2f * u);
    buttons[4] = (button){K_CTRL, rx, ly, 1.5f * u, "action", 0};
    buttons[5] = (button){K_ENTER, lx, 1.1f * u, 0.7f * u, "select", 0};
    buttons[6] = (button){K_ESC, rx, 1.1f * u, 0.7f * u, "back", 0};
    buttons[B_DROP] = (button){K_CTRL, rx, ly - 3.0f * u, 0.95f * u, "drop", 0};
}

static void drop_release(void) {
    if (!drop_pending) return;
    drop_pending = 0;
    rg_key(0, K_CTRL);
}

static void press(int b, int down) {
    if (buttons[b].down == down) return;
    buttons[b].down = down;
    if (b == B_DROP) {                       /* released by the main loop after DROP_MS */
        if (down) {
            if (!drop_pending) rg_key(1, K_CTRL);
            drop_pending = 1;
            drop_release_at = mono_ms() + DROP_MS;
        }
        return;
    }
    rg_key(down, buttons[b].key);
}

static int hit(float x, float y) {
    int best = -1;
    float bestd = 1e9f;
    for (int i = 0; i < NB; i++) {
        float dx = x - buttons[i].cx, dy = y - buttons[i].cy, dd = dx * dx + dy * dy;
        float reach = buttons[i].r * 1.35f;
        if (dd < reach * reach && dd < bestd) { best = i; bestd = dd; }
    }
    return best;
}

/* Soft-key labels drawn by the engine in the bottom corners of the game image act as
 * Select / Back when tapped. */
static int hit_softkeys(float x, float y) {
    if (y < BH - BH / 9) return -1;
    if (x >= game_x && x < game_x + 160) return 5;
    if (x >= game_x + GW * SCALE - 160 && x < game_x + GW * SCALE) return 6;
    return -1;
}

/* The controls are white shapes at various opacities, so they are rendered once into an
 * alpha mask (rebuilt only when a button changes state) and composited cheaply each frame. */
static uint8_t *mask;
static int mask_y0[2048], mask_y1[2048];  /* per row: x range containing coverage */
static unsigned mask_key = ~0u;           /* state the mask was built for */
static int dirty = 1;                     /* window content must be redrawn */

static void cover(int x, int y, int a) {
    if (x < 0 || x >= bw || y < 0 || y >= BH) return;
    uint8_t *m = &mask[y * bw + x];
    *m = (uint8_t)(a + *m * (255 - a) / 255);
    if (x < mask_y0[y]) mask_y0[y] = x;
    if (x > mask_y1[y]) mask_y1[y] = x;
}

static void mask_circle(float cx, float cy, float r, int alpha, int ring_only) {
    int x0 = (int)(cx - r - 1), x1 = (int)(cx + r + 1), y0 = (int)(cy - r - 1), y1 = (int)(cy + r + 1);
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            float d2 = (x - cx) * (x - cx) + (y - cy) * (y - cy);
            if (d2 > r * r) continue;
            if (ring_only && d2 < (r - 4) * (r - 4)) continue;
            cover(x, y, alpha);
        }
}

static void mask_triangle(float cx, float cy, float s, int dir, int alpha) {
    /* dir: 0 up 1 down 2 left 3 right */
    for (int y = (int)(cy - s); y <= (int)(cy + s); y++)
        for (int x = (int)(cx - s); x <= (int)(cx + s); x++) {
            float u = (x - cx) / s, v = (y - cy) / s;
            float along = dir == 0 ? -v : dir == 1 ? v : dir == 2 ? -u : u;
            float across = (dir < 2) ? u : v;
            if (along < -0.5f || along > 0.6f) continue;
            if (fabsf(across) > (0.6f - along) * 0.9f) continue;
            cover(x, y, alpha);
        }
}

static void build_mask(void) {
    unsigned key = (unsigned)controls_visible | (unsigned)bw << 1;
    for (int i = 0; i < NB; i++) key = key * 31 + (unsigned)buttons[i].down;
    if (key == mask_key && mask) return;
    mask_key = key;
    dirty = 1;
    free(mask);
    mask = (uint8_t *)calloc((size_t)bw * BH, 1);
    for (int y = 0; y < BH; y++) { mask_y0[y] = bw; mask_y1[y] = -1; }
    if (!controls_visible) return;
    for (int i = 0; i < NB; i++) {
        button *b = &buttons[i];
        mask_circle(b->cx, b->cy, b->r, b->down ? 150 : 70, 0);
        mask_circle(b->cx, b->cy, b->r, 140, 1);
        if (i < 4) mask_triangle(b->cx, b->cy, b->r * 0.55f, i, 200);
        else if (i == 4) mask_circle(b->cx, b->cy, b->r * 0.35f, 170, 0);
        else if (i == B_DROP) {
            /* a down arrow onto a bar: "place" */
            mask_triangle(b->cx, b->cy - b->r * 0.1f, b->r * 0.5f, 1, 200);
            for (int k = -(int)(b->r * 0.45f); k <= (int)(b->r * 0.45f); k++)
                for (int t = 0; t < 5; t++) cover((int)(b->cx + k), (int)(b->cy + b->r * 0.5f + t), 200);
        } else {
            /* select: horizontal bar; back: "x" */
            for (int k = -(int)(b->r * 0.4f); k <= (int)(b->r * 0.4f); k++)
                for (int t = -2; t <= 2; t++) {
                    if (i == 5) cover((int)(b->cx + k), (int)(b->cy + t), 200);
                    else {
                        cover((int)(b->cx + k), (int)(b->cy + k + t), 200);
                        cover((int)(b->cx + k), (int)(b->cy - k + t), 200);
                    }
                }
        }
    }
}

static void composite_controls(uint32_t *px, int stride) {
    for (int y = 0; y < BH; y++) {
        const uint8_t *m = mask + (size_t)y * bw;
        uint32_t *row = px + (size_t)y * stride;
        for (int x = mask_y0[y]; x <= mask_y1[y]; x++) {
            unsigned a = m[x];
            if (!a) continue;
            uint32_t d = row[x];
            uint32_t r = ((d & 0xff) * (255 - a) + 255 * a) / 255;
            uint32_t g = (((d >> 8) & 0xff) * (255 - a) + 255 * a) / 255;
            uint32_t b = (((d >> 16) & 0xff) * (255 - a) + 255 * a) / 255;
            row[x] = 0xff000000u | b << 16 | g << 8 | r;
        }
    }
}

/* ------------------------------------------------------------------ rendering */
static ANativeWindow *window;

static void setup_window(void) {
    int ww = ANativeWindow_getWidth(window), wh = ANativeWindow_getHeight(window);
    if (ww < wh) { int t = ww; ww = wh; wh = t; }
    bw = (int)((long)BH * ww / wh);
    if (bw < GW * SCALE) bw = GW * SCALE;
    bw &= ~1;
    game_x = (bw - GW * SCALE) / 2;
    ANativeWindow_setBuffersGeometry(window, bw, BH, WINDOW_FORMAT_RGBX_8888);
    layout_controls();
    mask_key = ~0u;
    dirty = 1;
    host_log("window %dx%d -> buffer %dx%d", ww, wh, bw, BH);
}

static uint32_t last_frame[GW * GH];

static void present(void) {
    const uint32_t *src = rg_pixels();
    build_mask();
    if (memcmp(last_frame, src, sizeof last_frame) != 0) { memcpy(last_frame, src, sizeof last_frame); dirty = 1; }
    if (!dirty || !window) return;
    ANativeWindow_Buffer buf;
    if (ANativeWindow_lock(window, &buf, NULL) != 0) return;
    if (buf.width != bw || buf.height != BH) { ANativeWindow_unlockAndPost(window); setup_window(); return; }
    uint32_t *px = (uint32_t *)buf.bits;
    for (int y = 0; y < BH; y++) {
        uint32_t *row = px + (size_t)y * buf.stride;
        memset(row, 0, (size_t)game_x * 4);
        memset(row + game_x + GW * SCALE, 0, (size_t)(bw - game_x - GW * SCALE) * 4);
        const uint32_t *s = last_frame + (y / SCALE) * GW;
        uint32_t *d = row + game_x;
        for (int x = 0; x < GW; x++) {
            uint32_t c = s[x];
            uint32_t v = 0xff000000u | (c & 0x00ff00u) | (c >> 16 & 0xff) | (c & 0xff) << 16;
            d[2 * x] = v;
            d[2 * x + 1] = v;
        }
    }
    composite_controls(px, buf.stride);
    ANativeWindow_unlockAndPost(window);
    /* Android may hand back an older buffer next time (double buffering): draw two frames */
    static int again;
    dirty = again = !again;
}

/* ------------------------------------------------------------------ input */
static int map_key(int32_t code) {
    switch (code) {
    case AKEYCODE_DPAD_UP: case AKEYCODE_W: return K_UP;
    case AKEYCODE_DPAD_DOWN: case AKEYCODE_S: return K_DOWN;
    case AKEYCODE_DPAD_LEFT: case AKEYCODE_A: return K_LEFT;
    case AKEYCODE_DPAD_RIGHT: case AKEYCODE_D: return K_RIGHT;
    case AKEYCODE_BUTTON_A: case AKEYCODE_DPAD_CENTER: case AKEYCODE_CTRL_LEFT: case AKEYCODE_CTRL_RIGHT:
    case AKEYCODE_SPACE: case AKEYCODE_BUTTON_R1: return K_CTRL;
    case AKEYCODE_BUTTON_START: case AKEYCODE_ENTER: case AKEYCODE_BUTTON_X: case AKEYCODE_BUTTON_L1: return K_ENTER;
    case AKEYCODE_BUTTON_B: case AKEYCODE_BACK: case AKEYCODE_ESCAPE: case AKEYCODE_BUTTON_SELECT: return K_ESC;
    default: return 0;
    }
}

static int32_t on_input(struct android_app *app, AInputEvent *ev) {
    (void)app;
    int type = AInputEvent_getType(ev);
    if (type == AINPUT_EVENT_TYPE_KEY) {
        int k = map_key(AKeyEvent_getKeyCode(ev));
        if (!k) return 0;
        int action = AKeyEvent_getAction(ev);
        if (AKeyEvent_getRepeatCount(ev) > 0) return 1;
        if (action == AKEY_EVENT_ACTION_DOWN || action == AKEY_EVENT_ACTION_UP) rg_key(action == AKEY_EVENT_ACTION_DOWN, k);
        if (AKeyEvent_getKeyCode(ev) != AKEYCODE_BACK) { controls_visible = 0; last_pad_input = mono_ms(); }
        return 1;
    }
    if (type == AINPUT_EVENT_TYPE_MOTION) {
        int src = AInputEvent_getSource(ev);
        if (src & AINPUT_SOURCE_JOYSTICK) {     /* analog stick / hat as d-pad */
            float hx = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_HAT_X, 0);
            float hy = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_HAT_Y, 0);
            float sx = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_X, 0);
            float sy = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_Y, 0);
            float x = fabsf(hx) > fabsf(sx) ? hx : sx, y = fabsf(hy) > fabsf(sy) ? hy : sy;
            press(2, x < -0.5f); press(3, x > 0.5f); press(0, y < -0.5f); press(1, y > 0.5f);
            controls_visible = 0;
            last_pad_input = mono_ms();
            return 1;
        }
        if (!window) return 0;
        controls_visible = 1;
        int action = AMotionEvent_getAction(ev);
        int masked = action & AMOTION_EVENT_ACTION_MASK;
        int idx = (action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
        int ww = ANativeWindow_getWidth(window), wh = ANativeWindow_getHeight(window);
        size_t n = AMotionEvent_getPointerCount(ev);
        for (size_t i = 0; i < n; i++) {
            int id = AMotionEvent_getPointerId(ev, i) & 15;
            float x = AMotionEvent_getX(ev, i) * bw / ww, y = AMotionEvent_getY(ev, i) * BH / wh;
            int up = (masked == AMOTION_EVENT_ACTION_UP || masked == AMOTION_EVENT_ACTION_CANCEL) ||
                     (masked == AMOTION_EVENT_ACTION_POINTER_UP && (int)i == idx);
            int b = up ? -1 : hit(x, y);
            if (b < 0 && !up && (masked == AMOTION_EVENT_ACTION_DOWN || masked == AMOTION_EVENT_ACTION_POINTER_DOWN) && (int)i == idx)
                b = hit_softkeys(x, y);
            if (b < 0 && !up && pointer_button[id] >= 4) b = pointer_button[id];   /* keep holding Action etc. */
            if (pointer_button[id] != b) {
                if (pointer_button[id] >= 0) press(pointer_button[id], 0);
                pointer_button[id] = b;
                if (b >= 0) press(b, 1);
            }
        }
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ lifecycle */
static int focused, booted;
static char data_dir[512];

static void on_cmd(struct android_app *app, int32_t cmd) {
    switch (cmd) {
    case APP_CMD_INIT_WINDOW:
        window = app->window;
        setup_window();
        break;
    case APP_CMD_TERM_WINDOW:
        window = NULL;
        break;
    case APP_CMD_GAINED_FOCUS:
        focused = 1;
        if (booted) { audio_start(); set_paused(0); }
        break;
    case APP_CMD_LOST_FOCUS:
    case APP_CMD_PAUSE:
        focused = 0;
        set_paused(1);
        audio_stop();
        for (int i = 0; i < NB; i++) if (buttons[i].down) press(i, 0);
        drop_release();
        break;
    case APP_CMD_WINDOW_RESIZED:
    case APP_CMD_CONFIG_CHANGED:
        if (window) setup_window();
        break;
    }
}

static void read_config(void) {
    /* optional files/revived.ini: server=ws://host:port/ws, nickname=Name */
    char path[600], line[256], server[200] = RG_DEFAULT_SERVER, nick[32] = "";
    snprintf(path, sizeof path, "%s/../revived.ini", data_dir);
    FILE *f = fopen(path, "r");
    while (f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "server=", 7)) snprintf(server, sizeof server, "%s", line + 7);
        if (!strncmp(line, "nickname=", 9)) snprintf(nick, sizeof nick, "%s", line + 9);
    }
    if (f) fclose(f);
    if (!nick[0]) {
        srand((unsigned)mono_ms());
        snprintf(nick, sizeof nick, "Droid%04d", rand() % 9000 + 1000);
        f = fopen(path, "w");
        if (f) { fprintf(f, "server=%s\nnickname=%s\n", server, nick); fclose(f); }
    }
    rg_net_configure(server, nick);
    host_log("online: %s as %s", server, nick);
}

void android_main(struct android_app *app) {
    ANativeActivity_setWindowFlags(app->activity, AWINDOW_FLAG_KEEP_SCREEN_ON | AWINDOW_FLAG_FULLSCREEN, 0);
    app->onAppCmd = on_cmd;
    app->onInputEvent = on_input;
    for (int i = 0; i < 16; i++) pointer_button[i] = -1;
    paused_at = mono_ms();               /* clock starts paused until the window has focus */
    snprintf(data_dir, sizeof data_dir, "%s/game", app->activity->internalDataPath);

    for (;;) {
        int events;
        struct android_poll_source *src;
        int timeout = (booted && focused && window) ? 0 : -1;
        if (!booted && window) timeout = 0;
        while (ALooper_pollOnce(timeout, NULL, &events, (void **)&src) >= 0) {
            if (src) src->process(app, src);
            if (app->destroyRequested) { audio_stop(); return; }
            timeout = (booted && focused && window) ? 0 : (!booted && window) ? 0 : -1;
        }
        if (!booted && window) {
            if (!install_data(app->activity->assetManager, data_dir, RG_DATA_STAMP)) host_fatal("game data missing from APK");
            read_config();
            char dll[600];
            snprintf(dll, sizeof dll, "%s/ResetGeneration-Win32-Web.dll", data_dir);
            if (!rg_init(dll, data_dir, GW, GH, "en")) host_fatal("engine failed to start");
            booted = 1;
            if (focused) { audio_start(); set_paused(0); }
        }
        if (booted && focused && window) {
            uint64_t t0 = mono_ms();
            if (drop_pending && t0 >= drop_release_at) drop_release();
            if (!rg_tick()) { ANativeActivity_finish(app->activity); }
            uint64_t t1 = mono_ms();
            audio_pump();
            if (!controls_visible && mono_ms() - last_pad_input > 30000) controls_visible = 1;
            present();
            static unsigned ticks;
            static uint64_t tick_ms, present_ms;
            tick_ms += t1 - t0;
            present_ms += mono_ms() - t1;
            if (++ticks % 1024 == 0) {
                host_log("tick %u: engine %llu ms, audio+present %llu ms per 1024 ticks, wait screen %d", ticks,
                         (unsigned long long)tick_ms, (unsigned long long)present_ms, rg_in_wait_screen());
                tick_ms = present_ms = 0;
            }
        }
    }
}
