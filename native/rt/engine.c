/* Drives the recompiled engine through its JNI entry points, the way the Java wrapper
 * (rg.ResetGeneration) did, and answers the engine's calls back into "Java". */
#include "engine.h"
#include "host.h"
#include "net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

static int W, H;
static uint32_t env, engine_obj, listener_obj, sound_obj, snap_obj, snap_listener_obj;
static uint32_t fb, sndbuf, sndbuf_h, netbuf, netbuf_h;
static uint32_t x_tick, x_key_down, x_key_up, x_sound, x_wait, x_volume;
#define SNDBUF_SIZE 4096
#define NETBUF_SIZE 2048

static uint32_t call(uint32_t fn, int n, ...) {
    uint32_t a[16];
    __builtin_va_list ap;
    __builtin_va_start(ap, n);
    for (int i = 0; i < n; i++) a[i] = __builtin_va_arg(ap, uint32_t);
    __builtin_va_end(ap);
    return rt_guest_call(fn, n, a);
}

void rg_use_virtual_clock(int on) { rt_set_virtual_time(on); }
void rg_advance_ms(uint32_t ms) { rt_advance_time(ms); }

static int cmp_str(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }

int rg_init(const char *dll_path, const char *game_dir, int width, int height, const char *lang) {
    static rt_image img;
    W = width;
    H = height;
    static char absdir[1024];
#ifdef _WIN32
    if (!_fullpath(absdir, game_dir, sizeof absdir)) snprintf(absdir, sizeof absdir, "%s", game_dir);
#else
    if (!realpath(game_dir, absdir)) snprintf(absdir, sizeof absdir, "%s", game_dir);
#endif
    game_dir = absdir;
    rt_mem_init();
    rt_dispatch_init();
    if (!rt_load_image(&img, dll_path, game_dir)) return 0;
    rt_jni_init();
    env = rt_jni_env();
    engine_obj = rt_jni_new_object("rg/natives/NativeGameEngine");
    listener_obj = rt_jni_new_object("rg/natives/NativeInputEventListener");
    sound_obj = rt_jni_new_object("rg/natives/NativeSoundOutputDevice");
    snap_obj = rt_jni_new_object("com/redlynx/mt/snapmobile/NativeSNAPMobileInterface");
    snap_listener_obj = rt_jni_new_object("com/redlynx/mt/snapmobile/NativeSNAPMobileEventListener");

    host_log("rg: DllMain");
    call(img.entry, 3, img.base, 1, 0);
    char home[1024];
#ifdef _WIN32
    if (!_fullpath(home, game_dir, sizeof home)) snprintf(home, sizeof home, "%s", game_dir);
#else
    if (!realpath(game_dir, home)) snprintf(home, sizeof home, "%s", game_dir);
#endif
    for (char *c = home; *c; c++) if (*c == '\\') *c = '/';
    call(rt_export("NativeGameEngine_setHomeDirectory"), 3, env, engine_obj, rt_jni_new_string(home));

    fb = rt_heap_alloc((uint32_t)(W * (H + 1) * 4));
    memset(M + fb, 0, (size_t)W * (H + 1) * 4);
    uint32_t fbh = rt_jni_new_direct_buffer(fb, (uint32_t)(W * (H + 1) * 4));
    host_log("rg: init %dx%d", W, H);
    call(rt_export("NativeGameEngine_init@"), 11, env, engine_obj, fbh, rt_jni_new_string(lang ? lang : "en"),
         0, W, H, rt_jni_new_string(""), 0, 0, 0);

    netbuf = rt_heap_alloc(NETBUF_SIZE);
    memset(M + netbuf, 0, NETBUF_SIZE);
    netbuf_h = rt_jni_new_direct_buffer(netbuf, NETBUF_SIZE);
    call(rt_export("NativeSNAPMobileInterface_initNative"), 5, env, snap_obj, netbuf_h, NETBUF_SIZE, 876);
    net_attach(M + netbuf, NETBUF_SIZE);

    /* data packages, in name order like the Java wrapper's directory listing */
    DIR *d = opendir(game_dir);
    char *names[64];
    int n = 0;
    struct dirent *e;
    while (d && (e = readdir(d)) && n < 64) {
        size_t l = strlen(e->d_name);
        if (l > 4 && !strcmp(e->d_name + l - 4, ".pak")) names[n++] = strdup(e->d_name);
    }
    if (d) closedir(d);
    qsort(names, (size_t)n, sizeof names[0], cmp_str);
    for (int i = 0; i < n; i++) {
        call(rt_export("NativeGameEngine_notifyDataPackageReady"), 3, env, engine_obj, rt_jni_new_string(names[i]));
        free(names[i]);
    }
    host_log("rg: %d data packages", n);

    sndbuf = rt_heap_alloc(SNDBUF_SIZE);
    sndbuf_h = rt_jni_new_direct_buffer(sndbuf, SNDBUF_SIZE);
    x_tick = rt_export("NativeGameEngine_tick");
    x_key_down = rt_export("NativeInputEventListener_onKeyPressed@");
    x_key_up = rt_export("NativeInputEventListener_onKeyReleased@");
    x_sound = rt_export("NativeSoundOutputDevice_onSoundBufferEmpty");
    x_wait = rt_export("NativeGameEngine_isInWaitScreen");
    x_volume = rt_export("NativeGameEngine_setVolume");
    return 1;
}

int rg_tick(void) {
    net_dispatch_events();
    return (int)(call(x_tick, 2, env, engine_obj) & 0xff);
}

const uint32_t *rg_pixels(void) { return (const uint32_t *)(M + fb); }

void rg_key(int down, int keycode) {
    call(down ? x_key_down : x_key_up, 6, env, listener_obj, rt_now_ms(), (uint32_t)keycode, 0, 1);
}

void rg_audio(uint8_t *buf, int len) {
    while (len > 0) {
        int n = len > SNDBUF_SIZE ? SNDBUF_SIZE : len;
        call(x_sound, 4, env, sound_obj, sndbuf_h, (uint32_t)n);
        memcpy(buf, M + sndbuf, (size_t)n);
        buf += n;
        len -= n;
    }
}

void rg_set_volume(int v) { call(x_volume, 3, env, engine_obj, (uint32_t)v); }
int rg_in_wait_screen(void) { return (int)(call(x_wait, 2, env, engine_obj) & 0xff); }

/* ------------------------------------------------------------------ online events -> engine */
void rg_ev_simple(const char *name) {
    char exp[96];
    snprintf(exp, sizeof exp, "NativeSNAPMobileEventListener_%s@", name);
    call(rt_export(exp), 2, env, snap_listener_obj);
}
void rg_ev_int(const char *name, int v) {
    char exp[96];
    snprintf(exp, sizeof exp, "NativeSNAPMobileEventListener_%s@", name);
    call(rt_export(exp), 3, env, snap_listener_obj, (uint32_t)v);
}
void rg_ev_str(const char *name, const char *s) {
    char exp[96];
    snprintf(exp, sizeof exp, "NativeSNAPMobileEventListener_%s@", name);
    call(rt_export(exp), 3, env, snap_listener_obj, rt_jni_new_string(s));
}
void rg_ev_data(int len, const char *room) {
    call(rt_export("NativeSNAPMobileEventListener_onDataReceived@"), 4, env, snap_listener_obj, (uint32_t)len,
         rt_jni_new_string(room));
}

/* ------------------------------------------------------------------ engine -> "Java" */
rt_jvalue rt_java_call(uint32_t obj, const char *name, const char *sig, const rt_jvalue *args, int nargs) {
    rt_jvalue r;
    r.j = 0;
    (void)nargs;
    if (obj == snap_obj) {
        const char *s0 = nargs > 0 ? rt_jni_string(args[0].l) : NULL;
        if (!strcmp(name, "init")) r.i = net_init();
        else if (!strcmp(name, "startLoginScreen")) net_start_login();
        else if (!strcmp(name, "stopLoginScreen")) net_stop_login();
        else if (!strcmp(name, "getLoginScreenStatus")) r.i = net_login_status();
        else if (!strcmp(name, "loginWasCancelled")) r.i = net_login_cancelled();
        else if (!strcmp(name, "isLoggedIn")) r.i = net_is_logged_in();
        else if (!strcmp(name, "login")) r.i = net_login();
        else if (!strcmp(name, "logout")) { if (getenv("RT_NET_DEBUG")) rt_dump_calltrace(); net_logout(); }
        else if (!strcmp(name, "createRoom")) { net_room("create", s0); r.l = rt_jni_new_string(s0 ? s0 : ""); }
        else if (!strcmp(name, "joinRoom")) net_room("join", s0);
        else if (!strcmp(name, "moveToRoom")) net_room("move", s0);
        else if (!strcmp(name, "leaveRoom")) net_leave(s0);
        else if (!strcmp(name, "send")) r.i = net_send(args[0].i);
        else if (!strcmp(name, "getFriendList")) net_friend_list();
        else { /* friends and presence: not supported by the relay yet */ }
        return r;
    }
    if (!strcmp(name, "startRecording")) return r;          /* microphone: not supported */
    host_log("java call %s%s ignored", name, sig);
    return r;
}

void rg_ev_friend_list_empty(void) {
    uint32_t a = rt_jni_new_object("array"), b = rt_jni_new_object("array"), c = rt_jni_new_object("array");
    call(rt_export("NativeSNAPMobileEventListener_onFriendList@"), 5, env, snap_listener_obj, a, b, c);
}
