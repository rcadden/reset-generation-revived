/* Internal interfaces between the runtime pieces, and the services each platform host
 * (native/host/desktop, native/android) must provide. */
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ provided by the platform */
void host_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void host_fatal(const char *msg) __attribute__((noreturn));
uint32_t host_time_ms(void);              /* timeGetTime(): a millisecond clock */
void host_sleep_ms(uint32_t ms);          /* Sleep() */

/* ------------------------------------------------------------------ runtime internals */
#define RT_STUB_BASE 0xF0000000u          /* fake code addresses of host imports / JNI functions */
extern uint32_t rt_last_call;

void rt_fatal(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
void rt_mem_init(void);
void rt_dump_calltrace(void);
void rt_mem_commit(uint32_t addr, uint32_t size);
void rt_dispatch_init(void);
uint32_t rt_guest_call(uint32_t addr, int nargs, const uint32_t *args);

/* Host stubs: guest-callable functions implemented in C. A stub reads its arguments with
 * rt_arg(i) and finishes with rt_ret(value, n) where n is the number of stack argument
 * slots the callee pops (stdcall) or 0 (cdecl). */
typedef void (*rt_stub_fn)(int arg);
uint32_t rt_stub_register(const char *name, rt_stub_fn fn, int arg);
void rt_stub_call(uint32_t addr);
const char *rt_stub_name(uint32_t addr);
static inline uint32_t rt_arg(int i);
void rt_ret(uint32_t eax, int pop_args);

/* guest heap (win32.c) */
uint32_t rt_heap_alloc(uint32_t n);
void rt_heap_free(uint32_t p);
uint32_t rt_heap_size(uint32_t p);
uint32_t rt_strdup_guest(const char *s);
const char *rt_guest_str(uint32_t p);     /* host view of a NUL-terminated guest string */

/* PE loader and Win32 layer (win32.c) */
typedef struct {
    uint32_t base, entry;
    const char *dir;                      /* host directory of the game data */
} rt_image;
int rt_load_image(rt_image *img, const char *dll_path, const char *game_dir);
uint32_t rt_export(const char *name_part);
void rt_set_virtual_time(int on);         /* deterministic clock for tests */
void rt_advance_time(uint32_t ms);
uint32_t rt_now_ms(void);

/* JNI layer (jni.c) */
void rt_jni_init(void);
uint32_t rt_jni_env(void);
uint32_t rt_jni_new_object(const char *kind);
uint32_t rt_jni_new_string(const char *s);
uint32_t rt_jni_new_direct_buffer(uint32_t guest_addr, uint32_t len);
const char *rt_jni_string(uint32_t handle);

/* Java methods the engine calls back (implemented by the engine glue in engine.c). */
typedef union { int32_t i; int64_t j; double d; uint32_t l; } rt_jvalue;
rt_jvalue rt_java_call(uint32_t obj, const char *name, const char *sig, const rt_jvalue *args, int nargs);

#include "rt.h"
static inline uint32_t rt_arg(int i) { return LD32(cpu.r[ESP] + 4 + 4 * (uint32_t)i); }

#ifdef __cplusplus
}
#endif
