/* Headless test driver for the recompiled engine (desktop).
 *
 *   rgtest <engine dll> <game data dir> <frames> <out dir> [--size WxH] [--keys "F:KEY,..."]
 *          [--shots F,F,...] [--every N] [--realtime]
 *
 * Runs the engine on a virtual clock (deterministic) unless --realtime, writes PPM screenshots
 * and a per-frame framebuffer hash log (out/hashes.txt), and reports timing. Keys are pressed
 * at frame F and released 6 frames later (F:KEY*N: N frames) (names: RETURN ESC UP DOWN LEFT RIGHT CTRL SPACE). */
#include "engine.h"
#include "host.h"
#include "server.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

void host_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

void host_fatal(const char *msg) {
    fprintf(stderr, "rgtest: fatal: %s\n", msg);
    fflush(stderr);
    exit(1);
}

#ifdef _WIN32
static LONG WINAPI on_crash(EXCEPTION_POINTERS *e) {
    uintptr_t a = (uintptr_t)e->ExceptionRecord->ExceptionInformation[1];
    fprintf(stderr, "CRASH: code %08lx at host pc %p, data address %p (guest %08llx), esp=%08x last stub=%08x\n",
            e->ExceptionRecord->ExceptionCode, e->ExceptionRecord->ExceptionAddress, (void *)a,
            (unsigned long long)(a - (uintptr_t)M), cpu.r[ESP], rt_last_call);
    fprintf(stderr, "  last stub %s, host pc offset 0x%llx\n", rt_stub_name(rt_last_call), (unsigned long long)((uintptr_t)e->ExceptionRecord->ExceptionAddress - (uintptr_t)GetModuleHandleA(NULL)));
    rt_dump_calltrace();
    if (getenv("RT_CRASH_STACK")) {   /* return addresses on the host stack (resolve with llvm-addr2line, image base 0x140000000) */
        uintptr_t base = (uintptr_t)GetModuleHandleA(NULL), *sp = (uintptr_t *)e->ContextRecord->Rsp;
        fprintf(stderr, "host stack:");
        for (int i = 0; i < 48; i++) if (sp[i] > base && sp[i] < base + 0x2000000) fprintf(stderr, " %llx", (unsigned long long)(sp[i] - base + 0x140000000ull));
        fputc(10, stderr);
    }
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

uint32_t host_time_ms(void) { return (uint32_t)(now_s() * 1000.0); }
void host_sleep_ms(uint32_t ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    usleep(ms * 1000);
#endif
}

static int keycode(const char *k) {
    static const struct { const char *n; int c; } t[] = {
        {"RETURN", 10}, {"ESC", 27}, {"UP", 38}, {"DOWN", 40}, {"LEFT", 37}, {"RIGHT", 39}, {"CTRL", 17}, {"SPACE", 32}};
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) if (!strcmp(t[i].n, k)) return t[i].c;
    return atoi(k);
}

static void save_ppm(const char *path, const uint32_t *px, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) {
        uint8_t rgb[3] = {(uint8_t)(px[i] >> 16), (uint8_t)(px[i] >> 8), (uint8_t)px[i]};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static uint64_t fnv(const uint32_t *px, size_t n) {
    uint64_t h = 1469598103934665603ull;
    const uint8_t *p = (const uint8_t *)px;
    for (size_t i = 0; i < n * 4; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

int main(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr, "usage: rgtest <dll> <game dir> <frames> <out dir> [--size WxH] [--keys F:KEY,...] [--shots F,...] [--every N] [--realtime]\n");
        return 2;
    }
    const char *dll = argv[1], *dir = argv[2], *out = argv[4];
    int frames = atoi(argv[3]), w = 640, h = 360, every = 0, realtime = 0;
    static int downs[100000], ups[100000];
    static char shot[100000];
    for (int i = 5; i < argc; i++) {
        if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &w, &h);
        else if (!strcmp(argv[i], "--every") && i + 1 < argc) every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--realtime")) realtime = 1;
        else if (!strcmp(argv[i], "--server") && i + 2 < argc) { rg_server_enable(argv[i + 1], argv[i + 2]); i += 2; }
        else if (!strcmp(argv[i], "--shots") && i + 1 < argc) {
            char *s = strdup(argv[++i]);
            for (char *t = strtok(s, ","); t; t = strtok(NULL, ",")) { int f = atoi(t); if (f >= 0 && f < 100000) shot[f] = 1; }
        } else if (!strcmp(argv[i], "--keys") && i + 1 < argc) {
            char *s = strdup(argv[++i]);
            for (char *t = strtok(s, ","); t; t = strtok(NULL, ",")) {
                int f = atoi(t);
                char *k = strchr(t, ':');
                int hold = 6;                      /* "F:KEY*N" holds the key for N frames */
                if (k && strchr(k, '*')) { hold = atoi(strchr(k, '*') + 1); *strchr(k, '*') = 0; }
                if (k && f >= 0 && hold > 0 && f + hold < 100000) { downs[f] = keycode(k + 1); ups[f + hold] = keycode(k + 1);
                    /* RG_REPEAT=1: like an OS keyboard, repeat the press while the key is held */
                    if (getenv("RG_REPEAT")) for (int r = f + 15; r < f + hold; r += 3) downs[r] = keycode(k + 1);
                }
            }
        }
    }
#ifdef _WIN32
    SetUnhandledExceptionFilter(on_crash);
#endif
    if (getenv("RG_NICK") || getenv("RG_SERVER"))
        rg_net_configure(getenv("RG_SERVER") ? getenv("RG_SERVER") : "ws://127.0.0.1:8787/ws", getenv("RG_NICK"));
    rg_use_virtual_clock(!realtime);
    double t0 = now_s();
    if (!rg_init(dll, dir, w, h, "en")) return 1;
    fprintf(stderr, "boot %.2fs\n", now_s() - t0);
    char path[1024];
    snprintf(path, sizeof path, "%s/hashes.txt", out);
    FILE *hf = fopen(path, "w");
    static uint8_t audio[1024];
    double t_engine = 0;
    int i;
    int cov_s[3] = {-1, -1, -1}, cov_e[3] = {-1, -1, -1};
    char cov_f[3][512] = {"", "", ""};
#ifdef RT_CALLTRACE
    /* RG_COV="start:end:file;..." records which engine functions are entered in a frame window */
    if (getenv("RG_COV")) {
        char *c = strdup(getenv("RG_COV"));
        int n = 0;
        for (char *t = strtok(c, ";"); t && n < 3; t = strtok(NULL, ";"), n++) sscanf(t, "%d:%d:%511s", &cov_s[n], &cov_e[n], cov_f[n]);
    }
#endif
    for (i = 0; i < frames; i++) {
#ifdef RT_CALLTRACE
        for (int n = 0; n < 3; n++) {
            if (i == cov_s[n]) { memset(rt_cov, 0, sizeof rt_cov); rt_cov_on = 1; }
            if (i == cov_e[n]) {
                rt_cov_on = 0;
                FILE *cf = fopen(cov_f[n], "w");
                if (cf) for (uint32_t a = 0; a < 0x200000; a++) if (rt_cov[a >> 3] & (1u << (a & 7))) fprintf(cf, "%08x\n", 0x10000000u + a);
                if (cf) fclose(cf);
            }
        }
#endif
        if (downs[i]) rg_key(1, downs[i]);
        if (ups[i]) rg_key(0, ups[i]);
        double t = now_s();
        if (!realtime) rg_advance_ms(1);
        int alive = rg_tick();
        if (i % 2 == 0) rg_audio(audio, sizeof audio);
        t_engine += now_s() - t;
        const uint32_t *px = rg_pixels();
        if (hf) fprintf(hf, "%d %016llx\n", i, (unsigned long long)fnv(px, (size_t)w * h));
        if (shot[i] || (every && i % every == 0) || i == frames - 1) {
            snprintf(path, sizeof path, "%s/frame_%05d.ppm", out, i);
            save_ppm(path, px, w, h);
        }
        if (!alive) { fprintf(stderr, "engine exit at frame %d\n", i); i++; break; }
    }
    if (hf) fclose(hf);
    fprintf(stderr, "%d frames, %.2f ms/frame engine time\n", i, 1000.0 * t_engine / (i ? i : 1));
    return 0;
}
