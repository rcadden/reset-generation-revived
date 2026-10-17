/* PE loader, guest heap and the Win32 functions the engine DLL imports (kernel32 + winmm).
 * Reference implementation: tools/emu/rgemu.py (the Unicorn oracle). */
#include "host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir_(p) _mkdir(p)
#else
#include <unistd.h>
#define mkdir_(p) mkdir(p, 0755)
#endif

/* guest address map */
#define STACK_BASE 0x00100000u
#define STACK_SIZE 0x00300000u
#define TEB_ADDR 0x00400000u
#define HEAP_BASE 0x20000000u
#define HEAP_SIZE 0x20000000u
#define VIRT_BASE 0x40000000u
#define VIRT_SIZE 0x20000000u

/* ------------------------------------------------------------------ stubs */
#define MAX_STUBS 1024
static struct { const char *name; rt_stub_fn fn; int arg; } stubs[MAX_STUBS];
static unsigned nstubs;

uint32_t rt_stub_register(const char *name, rt_stub_fn fn, int arg) {
    if (nstubs >= MAX_STUBS) rt_fatal("too many stubs");
    stubs[nstubs].name = name;
    stubs[nstubs].fn = fn;
    stubs[nstubs].arg = arg;
    return RT_STUB_BASE + 16 * ++nstubs;
}

const char *rt_stub_name(uint32_t addr) {
    uint32_t i = (addr - RT_STUB_BASE) / 16;
    return i >= 1 && i <= nstubs ? stubs[i - 1].name : "?";
}

void rt_stub_call(uint32_t addr) {
    uint32_t i = (addr - RT_STUB_BASE) / 16;
    if (i < 1 || i > nstubs || (addr & 15)) rt_fatal("call to bad stub address %08x", addr);
    if (!stubs[i - 1].fn) rt_fatal("unimplemented import %s", stubs[i - 1].name);
    static FILE *trace;
    static int trace_init;
    if (!trace_init) { const char *p = getenv("RT_API_TRACE"); trace = p ? fopen(p, "w") : NULL; trace_init = 1; }
    uint32_t esp0 = cpu.r[ESP];
    stubs[i - 1].fn(stubs[i - 1].arg);
    if (trace) {
        /* same format as tools/emu/rgemu.py --api-trace: "name result" ("-" for void) */
        const char *n = stubs[i - 1].name;
        if (!strncmp(n, "JNI_", 4)) { n = strchr(n + 4, '_') + 1; }
        fprintf(trace, "%s %x\n", n, cpu.r[EAX]);
        (void)esp0;
    }
}

void rt_ret(uint32_t eax, int pop_args) {
    cpu.r[EAX] = eax;
    cpu.r[ESP] += 4 + 4 * (uint32_t)pop_args;
}

/* ------------------------------------------------------------------ heap */
/* Blocks carry a 16-byte header (size, magic). Free blocks are kept in exact size classes:
 * 16-byte steps up to 4 KB, powers of two above. */
#define HDR 16
#define MAGIC 0x48454150u
#define NCLASS (256 + 32)
static uint32_t heap_top = HEAP_BASE;
static uint32_t free_head[NCLASS];

static unsigned size_class(uint32_t n, uint32_t *rounded) {
    if (n <= 4096) { uint32_t r = (n + 15) & ~15u; if (!r) r = 16; *rounded = r; return r / 16 - 1; }
    unsigned k = 13;
    while ((1u << k) < n) k++;
    *rounded = 1u << k;
    return 256 + (k - 13);
}

uint32_t rt_heap_alloc(uint32_t n) {
    uint32_t rounded;
    unsigned c = size_class(n, &rounded);
    uint32_t p = free_head[c];
    if (p) {
        free_head[c] = LD32(p);
    } else {
        if ((uint64_t)heap_top + HDR + rounded > (uint64_t)HEAP_BASE + HEAP_SIZE) rt_fatal("guest heap exhausted");
        p = heap_top + HDR;
        heap_top += HDR + rounded;
    }
    ST32(p - HDR, rounded);
    ST32(p - HDR + 4, MAGIC);
    return p;
}

void rt_heap_free(uint32_t p) {
    if (!p) return;
    if (LD32(p - HDR + 4) != MAGIC) { host_log("heap: free of non-heap pointer %08x ignored", p); return; }
    uint32_t rounded, size = LD32(p - HDR);
    unsigned c = size_class(size, &rounded);
    ST32(p - HDR + 4, 0);
    ST32(p, free_head[c]);
    free_head[c] = p;
}

uint32_t rt_heap_size(uint32_t p) {
    return LD32(p - HDR + 4) == MAGIC ? LD32(p - HDR) : 0xffffffffu;
}

uint32_t rt_strdup_guest(const char *s) {
    uint32_t n = (uint32_t)strlen(s) + 1;
    uint32_t p = rt_heap_alloc(n);
    memcpy(M + p, s, n);
    return p;
}

const char *rt_guest_str(uint32_t p) { return p ? (const char *)(M + p) : NULL; }

/* ------------------------------------------------------------------ time */
static int virtual_time;
static uint32_t vtime = 5000000;
void rt_set_virtual_time(int on) { virtual_time = on; }
void rt_advance_time(uint32_t ms) { vtime += ms; }
static uint32_t real_base;
uint32_t rt_now_ms(void) {
    if (virtual_time) return vtime;
    if (!real_base) real_base = host_time_ms() - 5000000u;   /* small, positive, like Windows uptime */
    return host_time_ms() - real_base;
}

/* ------------------------------------------------------------------ files */
static const char *game_dir;
#define MAX_FILES 64
static FILE *files[MAX_FILES];
#define FILE_H(i) (0x100u + 4u * (i))
#define MAX_FINDS 8
static struct { char dir[512]; char pat[128]; DIR *d; } finds[MAX_FINDS];
static uint32_t last_error;

static void host_path(const char *guest, char *out, size_t n) {
    char tmp[1024];
    size_t i;
    for (i = 0; guest[i] && i < sizeof tmp - 1; i++) tmp[i] = guest[i] == '\\' ? '/' : guest[i];
    tmp[i] = 0;
    int abs = tmp[0] == '/' || (tmp[0] && tmp[1] == ':');
    if (abs) snprintf(out, n, "%s", tmp);
    else snprintf(out, n, "%s/%s", game_dir, tmp);
}

static int wild_match(const char *pat, const char *s) {
    for (; *pat; pat++, s++) {
        if (*pat == '*') {
            for (;; s++) { if (wild_match(pat + 1, s)) return 1; if (!*s) return 0; }
        }
        if (!*s) return 0;
        if (*pat != '?' && tolower((unsigned char)*pat) != tolower((unsigned char)*s)) return 0;
    }
    return !*s;
}

/* ------------------------------------------------------------------ imports */
#define A(i) rt_arg(i)
#define IMPORT(name) static void w_##name(int arg_)

IMPORT(timeGetTime) { rt_ret(rt_now_ms(), 0); }
IMPORT(GetTickCount) { rt_ret(rt_now_ms(), 0); }
IMPORT(Sleep) {
    uint32_t ms = A(0);
    if (virtual_time) vtime += ms; else host_sleep_ms(ms);
    rt_ret(0, 1);
}
IMPORT(QueryPerformanceCounter) { uint64_t t = (uint64_t)rt_now_ms() * 1000; ST64(A(0), t); rt_ret(1, 1); }
IMPORT(GetSystemTimeAsFileTime) { ST64(A(0), 128000000000000000ull + (uint64_t)rt_now_ms() * 10000); rt_ret(0, 1); }
IMPORT(FileTimeToLocalFileTime) { ST64(A(1), LD64(A(0))); rt_ret(1, 2); }
IMPORT(FileTimeToSystemTime) {
    uint16_t st[8] = {2009, 5, 6, 30, 12, 0, 0, 0};
    memcpy(M + A(1), st, 16);
    rt_ret(1, 2);
}
IMPORT(GetTimeZoneInformation) { memset(M + A(0), 0, 172); rt_ret(0, 1); }

IMPORT(GetLastError) { rt_ret(last_error, 0); }
IMPORT(SetLastError) { last_error = A(0); rt_ret(0, 1); }
IMPORT(GetCurrentThreadId) { rt_ret(1, 0); }
IMPORT(GetCurrentProcessId) { rt_ret(1234, 0); }
IMPORT(GetCurrentProcess) { rt_ret(0xffffffffu, 0); }
IMPORT(ExitProcess) { rt_fatal("guest called ExitProcess(%u)", A(0)); }
IMPORT(TerminateProcess) { rt_fatal("guest called TerminateProcess(%u)", A(1)); }
IMPORT(RaiseException) {
    rt_fatal("guest RaiseException %08x (C++ exception; SEH dispatch not implemented)", A(0));
}
IMPORT(RtlUnwind) { rt_ret(0, 4); }
IMPORT(UnhandledExceptionFilter) { rt_ret(0, 1); }
IMPORT(SetUnhandledExceptionFilter) { rt_ret(0, 1); }

IMPORT(GetVersionExA) {
    uint32_t p = A(0);
    ST32(p + 4, 5); ST32(p + 8, 1); ST32(p + 12, 2600); ST32(p + 16, 2);   /* Windows XP */
    rt_ret(1, 1);
}
static uint32_t cmdline, env_a, env_w;
IMPORT(GetCommandLineA) { if (!cmdline) cmdline = rt_strdup_guest("java.exe"); rt_ret(cmdline, 0); }
IMPORT(GetEnvironmentStrings) {
    if (!env_a) { env_a = rt_heap_alloc(8); memcpy(M + env_a, "A=B\0\0", 5); }
    rt_ret(env_a, 0);
}
IMPORT(GetEnvironmentStringsW) {
    if (!env_w) { static const uint16_t w[] = {'A', '=', 'B', 0, 0}; env_w = rt_heap_alloc(16); memcpy(M + env_w, w, sizeof w); }
    rt_ret(env_w, 0);
}
IMPORT(FreeEnvironmentStringsA) { rt_ret(1, 1); }
IMPORT(FreeEnvironmentStringsW) { rt_ret(1, 1); }
IMPORT(SetEnvironmentVariableA) { rt_ret(1, 2); }
IMPORT(GetStartupInfoA) { memset(M + A(0), 0, 68); ST32(A(0), 68); rt_ret(0, 1); }
IMPORT(GetStdHandle) {
    uint32_t n = A(0);
    rt_ret(n == 0xfffffff6u ? 1 : n == 0xfffffff5u ? 2 : n == 0xfffffff4u ? 3 : 0xffffffffu, 1);
}
IMPORT(SetStdHandle) { rt_ret(1, 2); }
IMPORT(GetFileType) { rt_ret(2, 1); }
IMPORT(SetHandleCount) { rt_ret(32, 1); }
IMPORT(GetModuleHandleA) { rt_ret(0x10000000u, 1); }
IMPORT(GetModuleFileNameA) {
    char path[600];
    snprintf(path, sizeof path, "%s\\ResetGeneration-Win32-Web.dll", game_dir);
    for (char *c = path; *c; c++) if (*c == '/') *c = '\\';
    uint32_t n = A(2), len = (uint32_t)strlen(path);
    if (len >= n) len = n - 1;
    memcpy(M + A(1), path, len);
    M[A(1) + len] = 0;
    rt_ret(len, 3);
}
IMPORT(LoadLibraryA) { host_log("LoadLibraryA(%s) -> NULL", rt_guest_str(A(0))); rt_ret(0, 1); }
IMPORT(GetProcAddress) { rt_ret(0, 2); }
IMPORT(GetSystemInfo) {
    uint32_t p = A(0);
    memset(M + p, 0, 36);
    ST32(p + 4, 0x1000); ST32(p + 8, 0x10000); ST32(p + 12, 0x7ffeffff); ST32(p + 16, 1);
    ST32(p + 20, 1); ST32(p + 24, 586); ST32(p + 28, 0x10000);
    rt_ret(0, 1);
}
IMPORT(GetACP) { rt_ret(1252, 0); }
IMPORT(GetOEMCP) { rt_ret(437, 0); }
IMPORT(GetCPInfo) { uint32_t p = A(1); memset(M + p, 0, 20); ST32(p, 1); M[p + 4] = '?'; rt_ret(1, 2); }
IMPORT(GetLocaleInfoA) { rt_ret(0, 4); }

/* synchronisation: single-threaded guest */
IMPORT(InitializeCriticalSection) { rt_ret(0, 1); }
IMPORT(DeleteCriticalSection) { rt_ret(0, 1); }
IMPORT(EnterCriticalSection) { rt_ret(0, 1); }
IMPORT(LeaveCriticalSection) { rt_ret(0, 1); }
IMPORT(CreateMutexA) { rt_ret(0x50, 3); }
IMPORT(ReleaseMutex) { rt_ret(1, 1); }
IMPORT(CreateSemaphoreA) { rt_ret(0x51, 4); }
IMPORT(ReleaseSemaphore) { rt_ret(1, 3); }
IMPORT(WaitForSingleObject) { rt_ret(0, 2); }
IMPORT(InterlockedExchange) { uint32_t old = LD32(A(0)); ST32(A(0), A(1)); rt_ret(old, 2); }

/* TLS */
static uint32_t tls[64];
static uint32_t next_tls = 1;
IMPORT(TlsAlloc) { rt_ret(next_tls < 64 ? next_tls++ : 0xffffffffu, 0); }
IMPORT(TlsFree) { rt_ret(1, 1); }
IMPORT(TlsSetValue) { if (A(0) < 64) tls[A(0)] = A(1); rt_ret(1, 2); }
IMPORT(TlsGetValue) { last_error = 0; rt_ret(A(0) < 64 ? tls[A(0)] : 0, 1); }

/* memory */
static uint32_t virt_top = VIRT_BASE;
IMPORT(HeapCreate) { rt_ret(0x7000, 3); }
IMPORT(HeapDestroy) { rt_ret(1, 1); }
IMPORT(HeapAlloc) {
    uint32_t n = A(2), p = rt_heap_alloc(n);
    if (A(1) & 8) memset(M + p, 0, n);
    rt_ret(p, 3);
}
IMPORT(HeapFree) { rt_heap_free(A(2)); rt_ret(1, 3); }
IMPORT(HeapSize) { rt_ret(rt_heap_size(A(2)), 3); }
IMPORT(HeapReAlloc) {
    uint32_t flags = A(1), p = A(2), n = A(3);
    uint32_t old = p ? rt_heap_size(p) : 0;
    if (old == 0xffffffffu) old = 0;
    uint32_t q = rt_heap_alloc(n);
    if (p && old) { memcpy(M + q, M + p, old < n ? old : n); rt_heap_free(p); }
    if ((flags & 8) && n > old) memset(M + q + old, 0, n - old);
    rt_ret(q, 4);
}
IMPORT(VirtualAlloc) {
    uint32_t addr = A(0), size = A(1);
    if (addr && addr >= VIRT_BASE && addr < virt_top) { rt_ret(addr, 4); return; }
    uint32_t p = virt_top;
    virt_top += (size + 0xffff) & ~0xffffu;
    if (virt_top > VIRT_BASE + VIRT_SIZE) rt_fatal("guest VirtualAlloc space exhausted");
    rt_ret(p, 4);
}
IMPORT(VirtualFree) { rt_ret(1, 3); }
IMPORT(VirtualProtect) { if (A(3)) ST32(A(3), 4); rt_ret(1, 4); }
IMPORT(VirtualQuery) {
    uint32_t a = A(0), p = A(1);
    uint32_t v[7] = {a & ~0xfffu, a & ~0xffffu, 4, 0x1000, 0x1000, 4, 0x20000};
    memcpy(M + p, v, 28);
    rt_ret(28, 3);
}
IMPORT(IsBadReadPtr) { rt_ret(0, 2); }
IMPORT(IsBadWritePtr) { rt_ret(0, 2); }
IMPORT(IsBadCodePtr) { rt_ret(0, 1); }

/* files */
IMPORT(CreateFileA) {
    char path[1024];
    host_path(rt_guest_str(A(0)), path, sizeof path);
    uint32_t access = A(1), disp = A(4);
    int write = (access & 0x40000000u) != 0;
    FILE *f = NULL;
    if (write) {
        struct stat st;
        int exists = stat(path, &st) == 0;
        if (disp == 1 && exists) { last_error = 80; rt_ret(0xffffffffu, 7); return; }
        if (disp == 3 && !exists) { last_error = 2; rt_ret(0xffffffffu, 7); return; }
        f = fopen(path, (exists && (disp == 3 || disp == 4)) ? "r+b" : "w+b");
    } else {
        f = fopen(path, "rb");
    }
    if (!f) { host_log("CreateFileA: cannot open %s", path); last_error = 2; rt_ret(0xffffffffu, 7); return; }
    for (int i = 0; i < MAX_FILES; i++) {
        if (!files[i]) { files[i] = f; rt_ret(FILE_H(i), 7); return; }
    }
    fclose(f);
    last_error = 4;
    rt_ret(0xffffffffu, 7);
}

static FILE *file_of(uint32_t h) {
    uint32_t i = (h - 0x100u) / 4;
    return (h >= 0x100u && i < MAX_FILES) ? files[i] : NULL;
}

IMPORT(ReadFile) {
    FILE *f = file_of(A(0));
    uint32_t n = f ? (uint32_t)fread(M + A(1), 1, A(2), f) : 0;
    if (A(3)) ST32(A(3), n);
    rt_ret(1, 5);
}
IMPORT(WriteFile) {
    uint32_t h = A(0), n = A(2);
    if (h == 2 || h == 3) host_log("%.*s", (int)n, (const char *)(M + A(1)));
    else if (file_of(h)) fwrite(M + A(1), 1, n, file_of(h));
    if (A(3)) ST32(A(3), n);
    rt_ret(1, 5);
}
IMPORT(SetFilePointer) {
    FILE *f = file_of(A(0));
    if (!f) { rt_ret(0xffffffffu, 4); return; }
    fseek(f, (long)(int32_t)A(1), (int)A(3));
    if (A(2)) ST32(A(2), 0);
    rt_ret((uint32_t)ftell(f), 4);
}
IMPORT(SetEndOfFile) { rt_ret(1, 1); }
IMPORT(FlushFileBuffers) { FILE *f = file_of(A(0)); if (f) fflush(f); rt_ret(1, 1); }
IMPORT(CloseHandle) {
    uint32_t h = A(0), i = (h - 0x100u) / 4;
    if (h >= 0x100u && i < MAX_FILES && files[i]) { fclose(files[i]); files[i] = NULL; }
    rt_ret(1, 1);
}
IMPORT(DeleteFileA) { char p[1024]; host_path(rt_guest_str(A(0)), p, sizeof p); rt_ret(remove(p) == 0, 1); }
IMPORT(CreateDirectoryA) { char p[1024]; host_path(rt_guest_str(A(0)), p, sizeof p); mkdir_(p); rt_ret(1, 2); }
IMPORT(RemoveDirectoryA) { rt_ret(1, 1); }

static int find_fill(int k, uint32_t data) {
    struct dirent *e;
    while ((e = readdir(finds[k].d)) != NULL) {
        if (!wild_match(finds[k].pat, e->d_name)) continue;
        char full[1024];
        struct stat st;
        snprintf(full, sizeof full, "%s/%s", finds[k].dir, e->d_name);
        if (stat(full, &st) != 0) continue;
        memset(M + data, 0, 320);
        ST32(data, S_ISDIR(st.st_mode) ? 0x10 : 0x80);
        ST32(data + 28, 0);
        ST32(data + 32, (uint32_t)st.st_size);
        snprintf((char *)(M + data + 44), 260, "%s", e->d_name);
        return 1;
    }
    return 0;
}

IMPORT(FindFirstFileA) {
    char path[1024];
    host_path(rt_guest_str(A(0)), path, sizeof path);
    char *slash = strrchr(path, '/');
    int k;
    for (k = 0; k < MAX_FINDS && finds[k].d; k++) {}
    if (k == MAX_FINDS || !slash) { last_error = 2; rt_ret(0xffffffffu, 2); return; }
    *slash = 0;
    snprintf(finds[k].dir, sizeof finds[k].dir, "%s", path);
    snprintf(finds[k].pat, sizeof finds[k].pat, "%s", slash + 1);
    finds[k].d = opendir(finds[k].dir);
    if (!finds[k].d || !find_fill(k, A(1))) {
        if (finds[k].d) closedir(finds[k].d);
        finds[k].d = NULL;
        last_error = 2;
        rt_ret(0xffffffffu, 2);
        return;
    }
    rt_ret(0x900u + k, 2);
}
IMPORT(FindNextFileA) {
    uint32_t k = A(0) - 0x900u;
    if (k >= MAX_FINDS || !finds[k].d || !find_fill((int)k, A(1))) { last_error = 18; rt_ret(0, 2); return; }
    rt_ret(1, 2);
}
IMPORT(FindClose) {
    uint32_t k = A(0) - 0x900u;
    if (k < MAX_FINDS && finds[k].d) { closedir(finds[k].d); finds[k].d = NULL; }
    rt_ret(1, 1);
}

/* strings (Latin-1; the engine only uses these through the CRT locale code) */
IMPORT(MultiByteToWideChar) {
    uint32_t src = A(2), n = A(3), dst = A(4), dn = A(5);
    uint32_t len = n == 0xffffffffu ? (uint32_t)strlen(rt_guest_str(src)) + 1 : n;
    if (dn == 0) { rt_ret(len, 6); return; }
    uint32_t k = len < dn ? len : dn;
    for (uint32_t i = 0; i < k; i++) ST16(dst + 2 * i, M[src + i]);
    rt_ret(k, 6);
}
IMPORT(WideCharToMultiByte) {
    uint32_t src = A(2), n = A(3), dst = A(4), dn = A(5);
    uint32_t len = n;
    if (n == 0xffffffffu) { len = 0; while (LD16(src + 2 * len)) len++; len++; }
    if (dn == 0) { rt_ret(len, 8); return; }
    uint32_t k = len < dn ? len : dn;
    for (uint32_t i = 0; i < k; i++) { uint16_t c = LD16(src + 2 * i); M[dst + i] = c < 256 ? (uint8_t)c : '?'; }
    rt_ret(k, 8);
}
IMPORT(LCMapStringA) {
    uint32_t flags = A(1), src = A(2), n = A(3), dst = A(4), dn = A(5);
    uint32_t len = n == 0xffffffffu ? (uint32_t)strlen(rt_guest_str(src)) + 1 : n;
    if (dn == 0) { rt_ret(len, 6); return; }
    uint32_t k = len < dn ? len : dn;
    for (uint32_t i = 0; i < k; i++) {
        int c = M[src + i];
        M[dst + i] = (uint8_t)((flags & 0x100) ? tolower(c) : (flags & 0x200) ? toupper(c) : c);
    }
    rt_ret(k, 6);
}
IMPORT(LCMapStringW) {
    uint32_t flags = A(1), src = A(2), n = A(3), dst = A(4), dn = A(5);
    uint32_t len = n;
    if (n == 0xffffffffu) { len = 0; while (LD16(src + 2 * len)) len++; len++; }
    if (dn == 0) { rt_ret(len, 6); return; }
    uint32_t k = len < dn ? len : dn;
    for (uint32_t i = 0; i < k; i++) {
        uint16_t c = LD16(src + 2 * i);
        if (c < 128) c = (uint16_t)((flags & 0x100) ? tolower(c) : (flags & 0x200) ? toupper(c) : c);
        ST16(dst + 2 * i, c);
    }
    rt_ret(k, 6);
}
static uint16_t ctype_of(int c) {
    uint16_t v = 0;
    if (c < 128) {
        if (isupper(c)) v |= 1;
        if (islower(c)) v |= 2;
        if (isdigit(c)) v |= 4;
        if (isspace(c)) v |= 8;
        if (ispunct(c)) v |= 0x10;
        if (iscntrl(c)) v |= 0x20;
        if (c == ' ' || c == '\t') v |= 0x40;
        if (isalpha(c)) v |= 0x100;
    }
    return v;
}
IMPORT(GetStringTypeA) {
    uint32_t src = A(2), n = A(3), out = A(4);
    for (uint32_t i = 0; i < n; i++) ST16(out + 2 * i, ctype_of(M[src + i]));
    rt_ret(1, 5);
}
IMPORT(GetStringTypeW) {
    uint32_t src = A(1), n = A(2), out = A(3);
    for (uint32_t i = 0; i < n; i++) ST16(out + 2 * i, ctype_of(LD16(src + 2 * i)));
    rt_ret(1, 4);
}
static int cmp_ci(int a, int b, int ci) { return ci ? tolower(a) - tolower(b) : a - b; }
IMPORT(CompareStringA) {
    uint32_t flags = A(1), a = A(2), na = A(3), b = A(4), nb = A(5);
    if (na == 0xffffffffu) na = (uint32_t)strlen(rt_guest_str(a));
    if (nb == 0xffffffffu) nb = (uint32_t)strlen(rt_guest_str(b));
    int r = 0;
    for (uint32_t i = 0; !r && i < na && i < nb; i++) r = cmp_ci(M[a + i], M[b + i], flags & 1);
    if (!r) r = (int)na - (int)nb;
    rt_ret(r < 0 ? 1 : r > 0 ? 3 : 2, 6);
}
IMPORT(CompareStringW) {
    uint32_t flags = A(1), a = A(2), na = A(3), b = A(4), nb = A(5);
    if (na == 0xffffffffu) { na = 0; while (LD16(a + 2 * na)) na++; }
    if (nb == 0xffffffffu) { nb = 0; while (LD16(b + 2 * nb)) nb++; }
    int r = 0;
    for (uint32_t i = 0; !r && i < na && i < nb; i++) r = cmp_ci(LD16(a + 2 * i), LD16(b + 2 * i), flags & 1);
    if (!r) r = (int)na - (int)nb;
    rt_ret(r < 0 ? 1 : r > 0 ? 3 : 2, 6);
}

static const struct { const char *name; rt_stub_fn fn; } imports[] = {
#define I(n) {#n, w_##n}
    I(timeGetTime), I(GetTickCount), I(Sleep), I(QueryPerformanceCounter), I(GetSystemTimeAsFileTime),
    I(FileTimeToLocalFileTime), I(FileTimeToSystemTime), I(GetTimeZoneInformation), I(GetLastError),
    I(SetLastError), I(GetCurrentThreadId), I(GetCurrentProcessId), I(GetCurrentProcess), I(ExitProcess),
    I(TerminateProcess), I(RaiseException), I(RtlUnwind), I(UnhandledExceptionFilter),
    I(SetUnhandledExceptionFilter), I(GetVersionExA), I(GetCommandLineA), I(GetEnvironmentStrings),
    I(GetEnvironmentStringsW), I(FreeEnvironmentStringsA), I(FreeEnvironmentStringsW),
    I(SetEnvironmentVariableA), I(GetStartupInfoA), I(GetStdHandle), I(SetStdHandle), I(GetFileType),
    I(SetHandleCount), I(GetModuleHandleA), I(GetModuleFileNameA), I(LoadLibraryA), I(GetProcAddress),
    I(GetSystemInfo), I(GetACP), I(GetOEMCP), I(GetCPInfo), I(GetLocaleInfoA), I(InitializeCriticalSection),
    I(DeleteCriticalSection), I(EnterCriticalSection), I(LeaveCriticalSection), I(CreateMutexA),
    I(ReleaseMutex), I(CreateSemaphoreA), I(ReleaseSemaphore), I(WaitForSingleObject),
    I(InterlockedExchange), I(TlsAlloc), I(TlsFree), I(TlsSetValue), I(TlsGetValue), I(HeapCreate),
    I(HeapDestroy), I(HeapAlloc), I(HeapFree), I(HeapSize), I(HeapReAlloc), I(VirtualAlloc),
    I(VirtualFree), I(VirtualProtect), I(VirtualQuery), I(IsBadReadPtr), I(IsBadWritePtr),
    I(IsBadCodePtr), I(CreateFileA), I(ReadFile), I(WriteFile), I(SetFilePointer), I(SetEndOfFile),
    I(FlushFileBuffers), I(CloseHandle), I(DeleteFileA), I(CreateDirectoryA), I(RemoveDirectoryA),
    I(FindFirstFileA), I(FindNextFileA), I(FindClose), I(MultiByteToWideChar), I(WideCharToMultiByte),
    I(LCMapStringA), I(LCMapStringW), I(GetStringTypeA), I(GetStringTypeW), I(CompareStringA),
    I(CompareStringW),
#undef I
};

/* ------------------------------------------------------------------ PE loader */
static struct { char name[160]; uint32_t addr; } exports[64];
static int nexports;

uint32_t rt_export(const char *part) {
    for (int i = 0; i < nexports; i++) if (strstr(exports[i].name, part)) return exports[i].addr;
    rt_fatal("engine export *%s* not found", part);
}

static void setup_teb(void) {
    rt_mem_commit(TEB_ADDR, 0x2000);
    ST32(TEB_ADDR + 0x00, 0xffffffffu);           /* SEH chain end */
    ST32(TEB_ADDR + 0x04, STACK_BASE + STACK_SIZE);
    ST32(TEB_ADDR + 0x08, STACK_BASE);
    ST32(TEB_ADDR + 0x18, TEB_ADDR);
    ST32(TEB_ADDR + 0x24, 1);
    ST32(TEB_ADDR + 0x30, TEB_ADDR + 0x1000);     /* PEB */
    RF32(RF_FS_BASE) = TEB_ADDR;
}

int rt_load_image(rt_image *img, const char *dll_path, const char *dir) {
    game_dir = dir;
    img->dir = dir;
    FILE *f = fopen(dll_path, "rb");
    if (!f) { host_log("cannot open %s", dll_path); return 0; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = (uint8_t *)malloc((size_t)n);
    if (fread(d, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(d); return 0; }
    fclose(f);
#define U16(o) ((uint32_t)d[o] | (uint32_t)d[(o) + 1] << 8)
#define U32(o) (U16(o) | U16((o) + 2) << 16)
    uint32_t pe = U32(0x3c), nsec = U16(pe + 6), optsz = U16(pe + 20), opt = pe + 24;
    uint32_t base = U32(opt + 28), size = U32(opt + 56), hdrsize = U32(opt + 60);
    img->base = base;
    img->entry = base + U32(opt + 16);
    rt_mem_commit(STACK_BASE, STACK_SIZE);
    rt_mem_commit(HEAP_BASE, HEAP_SIZE);
    rt_mem_commit(VIRT_BASE, VIRT_SIZE);
    rt_mem_commit(base, size);
    memcpy(M + base, d, hdrsize);
    for (uint32_t i = 0; i < nsec; i++) {
        uint32_t s = opt + optsz + 40 * i;
        uint32_t vsize = U32(s + 8), va = U32(s + 12), rsize = U32(s + 16), rptr = U32(s + 20);
        memcpy(M + base + va, d + rptr, rsize < vsize ? rsize : vsize);
    }
    free(d);
    /* exports */
    uint32_t exp_rva = LD32(base + pe + 24 + 96);
    if (exp_rva) {
        uint32_t e = base + exp_rva;
        uint32_t nnames = LD32(e + 24), funcs = base + LD32(e + 28), names = base + LD32(e + 32), ords = base + LD32(e + 36);
        for (uint32_t i = 0; i < nnames && nexports < 64; i++) {
            snprintf(exports[nexports].name, sizeof exports[nexports].name, "%s", rt_guest_str(base + LD32(names + 4 * i)));
            exports[nexports].addr = base + LD32(funcs + 4 * LD16(ords + 2 * i));
            nexports++;
        }
    }
    /* imports: point every IAT slot at a host stub */
    uint32_t imp_rva = LD32(base + pe + 24 + 104);
    for (uint32_t desc = base + imp_rva; imp_rva && LD32(desc + 12); desc += 20) {
        uint32_t ilt = LD32(desc) ? base + LD32(desc) : base + LD32(desc + 16);
        uint32_t iat = base + LD32(desc + 16);
        for (uint32_t k = 0;; k++) {
            uint32_t ent = LD32(ilt + 4 * k);
            if (!ent) break;
            const char *name = (ent & 0x80000000u) ? "ordinal" : rt_guest_str(base + ent + 2);
            rt_stub_fn fn = NULL;
            for (size_t j = 0; j < sizeof imports / sizeof imports[0]; j++)
                if (!strcmp(imports[j].name, name)) fn = imports[j].fn;
            if (!fn) host_log("import %s not implemented (fails if called)", name);
            ST32(iat + 4 * k, rt_stub_register(name, fn, 0));
        }
    }
    setup_teb();
    cpu.r[ESP] = STACK_BASE + STACK_SIZE - 0x100;
    RF16(0x10a0) = 0x027f;                        /* x87 control word default */
    return 1;
}
