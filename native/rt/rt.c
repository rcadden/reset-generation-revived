/* Runtime core: guest memory, CPU state, indirect-call dispatch, helpers for generated code. */
#include "rt.h"
#include "host.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

rt_cpu cpu;
#ifdef RT_CALLTRACE
uint32_t rt_calltrace[256];
unsigned rt_calltrace_pos;
int rt_trace_live;
int rt_cov_on;
uint8_t rt_cov[0x40000];
uint32_t rt_callstack[1024];
unsigned rt_callstack_depth;
void rt_trace_live_log(uint32_t a) {
    rt_trace_live--;
    host_log("  live %08x ecx=%08x a1=%08x a2=%08x", a, cpu.r[ECX], LD32(cpu.r[ESP] + 4), LD32(cpu.r[ESP] + 8));
}
#endif
uint8_t *M;

/* ------------------------------------------------------------------ memory */
#ifdef _WIN32
#include <windows.h>
static void *reserve(size_t n) { return VirtualAlloc(NULL, n, MEM_RESERVE, PAGE_NOACCESS); }
static int commit(void *p, size_t n) { return VirtualAlloc(p, n, MEM_COMMIT, PAGE_READWRITE) != NULL; }
#else
#include <sys/mman.h>
static void *reserve(size_t n) {
    void *p = mmap(NULL, n, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}
static int commit(void *p, size_t n) { return mprotect(p, n, PROT_READ | PROT_WRITE) == 0; }
#endif

void rt_mem_init(void) {
    M = (uint8_t *)reserve((size_t)1 << 32);
    if (!M) rt_fatal("cannot reserve 4 GB of address space for guest memory");
}

void rt_mem_commit(uint32_t addr, uint32_t size) {
    uint32_t start = addr & ~0xfffu;
    uint64_t end = ((uint64_t)addr + size + 0xfff) & ~(uint64_t)0xfff;
    if (!commit(M + start, (size_t)(end - start))) rt_fatal("cannot commit guest memory %08x+%x", addr, size);
}

/* ------------------------------------------------------------------ errors */
void rt_dump_calltrace(void) {
#ifdef RT_CALLTRACE
    char line[256 * 9 + 1];
    size_t n = 0;
    for (unsigned i = 48; i > 0; i--) n += (size_t)snprintf(line + n, sizeof line - n, "%08x ", rt_calltrace[(rt_calltrace_pos - i) & 255]);
    host_log("last function entries (oldest first): %s", line);
    n = 0;
    for (unsigned i = 0; i < rt_callstack_depth && i < 1024 && n < sizeof line - 10; i++)
        n += (size_t)snprintf(line + n, sizeof line - n, "%08x ", rt_callstack[i]);
    host_log("guest call stack (outermost first): %s", line);
#endif
}

void rt_fatal(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    host_log("FATAL: %s (eip-ish: last call %08x, esp %08x)", buf, rt_last_call, cpu.r[ESP]);
    rt_dump_calltrace();
    host_fatal(buf);
    abort();
}

void rt_div0(void) { rt_fatal("integer division by zero"); }
void rt_bad_branch(uint32_t func, uint32_t target) { rt_fatal("branch to unknown code %08x from function %08x", target, func); }
void rt_unimpl(uint32_t addr, const char *what) { rt_fatal("unsupported instruction at %08x (%s)", addr, what); }

/* ------------------------------------------------------------------ dispatch */
uint32_t rt_last_call;
static uint32_t *hkeys;
static void (**hfns)(void);
static uint32_t hmask;

static uint32_t hash32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}

void rt_dispatch_init(void) {
    uint32_t n = 1;
    while (n < rt_func_count * 4) n <<= 1;
    hmask = n - 1;
    hkeys = (uint32_t *)calloc(n, sizeof *hkeys);
    hfns = (void (**)(void))calloc(n, sizeof *hfns);
    for (unsigned i = 0; i < rt_func_count; i++) {
        uint32_t h = hash32(rt_func_table[i].addr) & hmask;
        while (hkeys[h]) h = (h + 1) & hmask;
        hkeys[h] = rt_func_table[i].addr;
        hfns[h] = rt_func_table[i].fn;
    }
}

void rt_call(uint32_t addr) {
    uint32_t h = hash32(addr) & hmask;
    for (;;) {
        uint32_t k = hkeys[h];
        if (k == addr) { hfns[h](); return; }
        if (!k) break;
        h = (h + 1) & hmask;
    }
    if (addr >= RT_STUB_BASE) {
        rt_last_call = addr;
        rt_stub_call(addr);
        return;
    }
    rt_fatal("indirect call to unknown address %08x", addr);
}

void rt_trace_ret(uint32_t func, uint32_t eax) { host_log("   ret %08x -> %08x", func, eax); }

void rt_trace(uint32_t func) {
    host_log("trace %08x ecx=%08x [esp+4]=%08x [esp+8]=%08x", func, cpu.r[ECX], LD32(cpu.r[ESP] + 4), LD32(cpu.r[ESP] + 8));
    if (func == 0x100d4df0) {   /* place action: cursor, rotation vs the tutorial's forced target */
        uint32_t t = cpu.r[ECX], f = LD32(t + 0x18);
        host_log("   DROP cursor=(%d,%d) rot=%d | forced=%d at (%d,%d) rot=%d",
                 (int)LD32(t + 0x84), (int)LD32(t + 0x88), (int)LD32(t + 0xb4),
                 (int)(LD32(f + 0xac) >> 8 & 0xff), (int)LD32(f + 0x98), (int)LD32(f + 0x9c), (int)LD32(f + 0xa8));
    }
    if (func == 0x100078e0) {   /* place-tile order: owner, rotation, piece, position */
        uint32_t o = LD32(cpu.r[ESP] + 4);
        host_log("   PLACE order %08x owner=%d rot=%d piece=%d pos=(%d,%d)", o, (int)LD32(o + 0x30), (int)LD32(o + 0x4c), (int)LD32(o + 0x50), (int)LD32(o + 0x44), (int)LD32(o + 0x48));
    }
    if (func == 0x1000ce60) {
        host_log("   event type %d", (int)LD32(cpu.r[ECX] + 0x18));
#ifdef RT_CALLTRACE
        if (LD32(cpu.r[ECX] + 0x18) == 13 || LD32(cpu.r[ECX] + 0x18) == 5) {
            char line[1024]; size_t n = 0;
            for (unsigned i = 60; i > 0; i--) n += (size_t)snprintf(line + n, sizeof line - n, "%08x ", rt_calltrace[(rt_calltrace_pos - i) & 255]);
            host_log("   calls: %s", line);
        }
#endif
    }
    if (func == 0x10115040 || func == 0x10114370) {   /* key/value lookup: print the key */
        uint32_t a = LD32(cpu.r[ESP] + 4), h = a ? LD32(a) : 0;
        if (func == 0x10114370) host_log("   kv get \"%s\"", (const char *)(M + a));
        else if (h) host_log("   kv key \"%.*s\"", (int)(LD32(h + 4) & 0xff), (const char *)(M + LD32(h + 0xc)));
    }
#ifdef RT_CALLTRACE
    {   /* RT_LIVE_TRIGGER=<hex addr>:<count> logs the next <count> function entries after it */
        static uint32_t trig, count, inited;
        if (!inited) { const char *e = getenv("RT_LIVE_TRIGGER"); if (e) sscanf(e, "%x:%u", &trig, &count); inited = 1; }
        if (trig == func && rt_trace_live <= 0) rt_trace_live = (int)count;
    }
#endif
    if (func == 0x10110210) {   /* MT connect(address): dump the address object */
        uint32_t a = LD32(cpu.r[ESP] + 4), self = cpu.r[ECX], cur = LD32(self + 0x3c);
        uint32_t h = LD32(a + 0xc);
        host_log("   connect addr %08x vt %08x type %u str \"%.*s\" | current %08x type %u", a, LD32(a), LD32(a + 4),
                 (int)(LD32(h + 4) & 0xff), (const char *)(M + LD32(h + 0xc)), cur, cur ? LD32(cur + 4) : 0);
    }
    if (func == 0x101107d0) {   /* event broadcast: list the listeners' handlers */
        uint32_t bit = LD32(cpu.r[ESP] + 4), k = 0;
        while (!(bit & 1) && k < 32) { bit >>= 1; k++; }
        uint32_t list = LD32(cpu.r[ECX] + 4 * k + 4);
        for (uint32_t n = list ? LD32(list) : 0; n; n = LD32(n + 4)) {
            uint32_t l = LD32(n + 8);
            host_log("   listener %08x vtable %08x handler %08x slot %08x %08x %08x", l, LD32(l), LD32(LD32(l) + 8), LD32(l + k * 12 + 0xc), LD32(l + k * 12 + 0x10), LD32(l + k * 12 + 0x14));
        }
    }
}

static struct { uint32_t func; rt_hook_fn fn; } hooks[16];
static int nhooks;
void rt_hook_add(uint32_t func, rt_hook_fn fn) {
    if (nhooks >= 16) rt_fatal("too many hooks");
    hooks[nhooks].func = func;
    hooks[nhooks++].fn = fn;
}
void rt_hook(uint32_t func) {
    for (int i = 0; i < nhooks; i++) if (hooks[i].func == func) hooks[i].fn();
}

/* Calls guest function `addr` from the host with stdcall/cdecl arguments; returns EAX. */
uint32_t rt_guest_call(uint32_t addr, int nargs, const uint32_t *args) {
    uint32_t saved_esp = cpu.r[ESP];
    uint32_t esp = saved_esp - 64;          /* keep clear of anything the caller left below ESP */
    for (int i = nargs - 1; i >= 0; i--) { esp -= 4; ST32(esp, args[i]); }
    esp -= 4;
    ST32(esp, RT_STUB_BASE);                /* return address: never executed */
    cpu.r[ESP] = esp;
    rt_call(addr);
    cpu.r[ESP] = saved_esp;
    return cpu.r[EAX];
}

uint32_t rt_guest_thiscall(uint32_t addr, uint32_t ecx, int nargs, const uint32_t *args) {
    uint32_t saved_ecx = cpu.r[ECX];
    cpu.r[ECX] = ecx;
    uint32_t r = rt_guest_call(addr, nargs, args);
    cpu.r[ECX] = saved_ecx;
    return r;
}

/* ------------------------------------------------------------------ helpers */
void rt_movs(uint32_t dst, uint32_t src, uint32_t n, int w, int backward) {
    if (!n) return;
    if (backward) {
        /* DF=1: element-wise from the given addresses downwards. */
        uint32_t d = dst + w - n, s = src + w - n;
        memmove(M + d, M + s, n);
        return;
    }
    if (dst <= src || dst >= src + n) {
        memmove(M + dst, M + src, n);
    } else {
        /* overlapping forward copy: x86 semantics replicate the pattern */
        for (uint32_t i = 0; i < n; i++) M[dst + i] = M[src + i];
    }
}

void rt_stos(uint32_t dst, uint32_t val, uint32_t count, int w, int backward) {
    if (!count) return;
    uint32_t n = count * w;
    uint32_t start = backward ? dst + w - n : dst;
    if (w == 1) { memset(M + start, (int)(val & 0xff), n); return; }
    for (uint32_t i = 0; i < count; i++) {
        if (w == 2) ST16(start + i * 2, (uint16_t)val);
        else ST32(start + i * 4, val);
    }
}

rt_cpuid_t rt_cpuid(uint32_t leaf, uint32_t sub) {
    (void)sub;
    rt_cpuid_t r = {0, 0, 0, 0};
    if (leaf == 0) {                        /* "GenuineIntel", max leaf 1 */
        r.a = 1; r.b = 0x756e6547; r.d = 0x49656e69; r.c = 0x6c65746e;
    } else if (leaf == 1) {                 /* Pentium-class: FPU, TSC, CMOV; no MMX/SSE/SSE2 */
        r.a = 0x00000633; r.d = (1u << 0) | (1u << 4) | (1u << 8) | (1u << 15);
    }
    return r;
}

/* 80-bit x87 extended <-> double */
double rt_ext80_to_double(const uint8_t *p) {
    uint64_t mant; uint16_t se;
    memcpy(&mant, p, 8); memcpy(&se, p + 8, 2);
    int sign = se >> 15, exp = se & 0x7fff;
    if (exp == 0 && mant == 0) return sign ? -0.0 : 0.0;
    if (exp == 0x7fff) return (mant << 1) ? NAN : (sign ? -INFINITY : INFINITY);
    double d = ldexp((double)mant, exp - 16383 - 63);
    return sign ? -d : d;
}

void rt_double_to_ext80(uint8_t *p, double d) {
    uint64_t mant = 0; uint16_t se = 0;
    if (signbit(d)) { se = 0x8000; d = -d; }
    if (isnan(d)) { se |= 0x7fff; mant = 0xc000000000000000ull; }
    else if (isinf(d)) { se |= 0x7fff; mant = 0x8000000000000000ull; }
    else if (d != 0.0) {
        int e;
        double f = frexp(d, &e);           /* d = f * 2^e, 0.5 <= f < 1 */
        mant = (uint64_t)ldexp(f, 64);
        se |= (uint16_t)(e - 1 + 16383);
    }
    memcpy(p, &mant, 8); memcpy(p + 8, &se, 2);
}
