/* Runtime interface for the statically recompiled engine (see tools/recomp/recomp.py).
 *
 * Guest memory is one flat 32-bit address space starting at host pointer M; the generated
 * code reads and writes it with LDn/STn. CPU state that crosses function boundaries lives
 * in `cpu`: the eight general-purpose registers in r[], everything else (x87 stack, DF,
 * FPU control/status, FS base, ...) in the register file rf[], indexed by Ghidra's x86
 * register offsets.
 *
 * Build with -fno-strict-aliasing -fwrapv. */
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI };

typedef struct {
    uint32_t r[8];
    uint8_t rf[0x1400] __attribute__((aligned(16)));
} rt_cpu;

extern rt_cpu cpu;
extern uint8_t *M;

/* ------------------------------------------------------------------ guest memory */
static inline uint8_t LD8(uint32_t a) { return M[a]; }
static inline uint16_t LD16(uint32_t a) { uint16_t v; memcpy(&v, M + a, 2); return v; }
static inline uint32_t LD32(uint32_t a) { uint32_t v; memcpy(&v, M + a, 4); return v; }
static inline uint64_t LD64(uint32_t a) { uint64_t v; memcpy(&v, M + a, 8); return v; }
static inline void ST8(uint32_t a, uint8_t v) { M[a] = v; }
static inline void ST16(uint32_t a, uint16_t v) { memcpy(M + a, &v, 2); }
static inline void ST32(uint32_t a, uint32_t v) { memcpy(M + a, &v, 4); }
static inline void ST64(uint32_t a, uint64_t v) { memcpy(M + a, &v, 8); }
double rt_ext80_to_double(const uint8_t *p);
void rt_double_to_ext80(uint8_t *p, double d);
static inline double LDX(uint32_t a) { return rt_ext80_to_double(M + a); }
static inline void STX(uint32_t a, double d) { rt_double_to_ext80(M + a, d); }

/* ------------------------------------------------------------------ register file */
#define RF8(o) (*(uint8_t *)&cpu.rf[o])
#define RF16(o) (*(uint16_t *)&cpu.rf[o])
#define RF32(o) (*(uint32_t *)&cpu.rf[o])
#define RF64(o) (*(uint64_t *)&cpu.rf[o])
#define RFD(o) (*(double *)&cpu.rf[o])      /* 80-bit x87 registers held as doubles */
#define RF_DF 0x20a
#define RF_FS_BASE 0x110

/* ------------------------------------------------------------------ flags */
#define CARRY8(a, b) ((uint8_t)((uint8_t)((a) + (b)) < (uint8_t)(a)))
#define CARRY16(a, b) ((uint8_t)((uint16_t)((a) + (b)) < (uint16_t)(a)))
#define CARRY32(a, b) ((uint8_t)((uint32_t)((a) + (b)) < (uint32_t)(a)))
#define CARRY64(a, b) ((uint8_t)((uint64_t)((a) + (b)) < (uint64_t)(a)))
#define RT_SCARRY(T, bits) \
    static inline uint8_t SCARRY##bits(T a, T b) { T r = (T)(a + b); return (uint8_t)((((a ^ r) & (b ^ r)) >> (bits - 1)) & 1); } \
    static inline uint8_t SBORROW##bits(T a, T b) { T r = (T)(a - b); return (uint8_t)((((a ^ b) & (a ^ r)) >> (bits - 1)) & 1); }
RT_SCARRY(uint8_t, 8)
RT_SCARRY(uint16_t, 16)
RT_SCARRY(uint32_t, 32)
RT_SCARRY(uint64_t, 64)

static inline uint32_t LZCNT8(uint8_t x) { return x ? (uint32_t)__builtin_clz(x) - 24 : 8; }
static inline uint32_t LZCNT16(uint16_t x) { return x ? (uint32_t)__builtin_clz(x) - 16 : 16; }
static inline uint32_t LZCNT32(uint32_t x) { return x ? (uint32_t)__builtin_clz(x) : 32; }
static inline uint32_t LZCNT64(uint64_t x) { return x ? (uint32_t)__builtin_clzll(x) : 64; }

void rt_div0(void);
#define RT_NZ(x) ({ __typeof__(x) nz_ = (x); if (!nz_) rt_div0(); nz_; })

/* ------------------------------------------------------------------ floats */
static inline float F32(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }
static inline double F64(uint64_t b) { double f; memcpy(&f, &b, 8); return f; }
static inline uint32_t B32(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }
static inline uint64_t B64(double f) { uint64_t b; memcpy(&b, &f, 8); return b; }
/* x86 float->int conversions give the "integer indefinite" value when out of range. */
static inline uint16_t F2I16(double d) { return (d > -32769.0 && d < 32768.0) ? (uint16_t)(int16_t)d : 0x8000; }
static inline uint32_t F2I32(double d) { return (d > -2147483649.0 && d < 2147483648.0) ? (uint32_t)(int32_t)d : 0x80000000u; }
static inline uint64_t F2I64(double d) { return (d >= -9223372036854775808.0 && d < 9223372036854775808.0) ? (uint64_t)(int64_t)d : 0x8000000000000000ull; }

/* ------------------------------------------------------------------ string ops and CPUID */
void rt_movs(uint32_t dst, uint32_t src, uint32_t n, int w, int backward);
void rt_stos(uint32_t dst, uint32_t val, uint32_t count, int w, int backward);
#define RT_REP_MOVS(w) do { uint32_t n_ = ecx * (w); int bk_ = RF8(RF_DF); rt_movs(edi, esi, n_, (w), bk_); \
    if (bk_) { esi -= n_; edi -= n_; } else { esi += n_; edi += n_; } ecx = 0; } while (0)
#define RT_REP_STOS(w) do { uint32_t n_ = ecx * (w); int bk_ = RF8(RF_DF); rt_stos(edi, eax, ecx, (w), bk_); \
    if (bk_) edi -= n_; else edi += n_; ecx = 0; } while (0)
typedef struct { uint32_t a, b, c, d; } rt_cpuid_t;
rt_cpuid_t rt_cpuid(uint32_t leaf, uint32_t sub);
#define RT_CPUID() do { rt_cpuid_t c_ = rt_cpuid(eax, ecx); eax = c_.a; ebx = c_.b; ecx = c_.c; edx = c_.d; } while (0)

/* ------------------------------------------------------------------ control flow */
/* -DRT_CALLTRACE keeps a ring buffer of the last function entries (crash diagnostics). */
#ifdef RT_CALLTRACE
extern uint32_t rt_calltrace[256];
extern unsigned rt_calltrace_pos;
extern int rt_trace_live;
void rt_trace_live_log(uint32_t a);
extern uint32_t rt_callstack[1024];
extern unsigned rt_callstack_depth;
#define RT_ENTER(a) (rt_calltrace[rt_calltrace_pos++ & 255] = (a), rt_callstack[rt_callstack_depth++ & 1023] = (a),                      rt_trace_live > 0 ? rt_trace_live_log(a) : (void)0)
#define RT_LEAVE() (rt_callstack_depth--)
#else
#define RT_ENTER(a) ((void)0)
#define RT_LEAVE() ((void)0)
#endif
typedef struct { uint32_t addr; void (*fn)(void); } rt_func_entry;
extern const rt_func_entry rt_func_table[];
extern const unsigned rt_func_count;

void rt_call(uint32_t addr);                               /* indirect call/jump target */
void rt_trace(uint32_t func);                              /* recomp.py --trace */
void rt_hook(uint32_t func);                               /* recomp.py hooks (DEFAULT_HOOKS / --hook) */
typedef void (*rt_hook_fn)(void);                          /* runs at function entry; ECX/ESP are the callee's */
void rt_hook_add(uint32_t func, rt_hook_fn fn);
void rt_bad_branch(uint32_t func, uint32_t target) __attribute__((noreturn));
void rt_unimpl(uint32_t addr, const char *what) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif
