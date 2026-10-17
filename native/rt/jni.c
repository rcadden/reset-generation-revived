/* A fake JNIEnv for the engine: the function table lives in guest memory and points at host
 * stubs. Java objects are small integer handles into a host-side table. The engine's calls
 * back into "Java" (CallXxxMethod) are forwarded to rt_java_call() by method name. */
#include "host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JNI_BASE 0x00500000u
#define OBJ_BASE 0x1000u
#define MAX_OBJS 8192

enum { O_FREE, O_CLASS, O_OBJECT, O_STRING, O_DBUF, O_METHOD, O_FIELD };
typedef struct {
    int kind;
    char *str;          /* class name, string value, method name */
    char *sig;          /* method signature */
    uint32_t addr, len; /* direct buffer */
} jobj;
static jobj objs[MAX_OBJS];
static uint32_t nobj = 1;

static uint32_t new_obj(int kind, const char *str, const char *sig) {
    /* Strings are created per call (NewStringUTF in loops); recycle freed slots. */
    uint32_t i;
    for (i = nobj; i < MAX_OBJS && objs[i].kind != O_FREE; i++) {}
    if (i >= MAX_OBJS) for (i = 1; i < MAX_OBJS && objs[i].kind != O_FREE; i++) {}
    if (i >= MAX_OBJS) rt_fatal("JNI object table full");
    objs[i].kind = kind;
    objs[i].str = str ? strdup(str) : NULL;
    objs[i].sig = sig ? strdup(sig) : NULL;
    objs[i].addr = objs[i].len = 0;
    nobj = i + 1 < MAX_OBJS ? i + 1 : 1;
    return OBJ_BASE + 4 * i;
}

static jobj *obj(uint32_t h) {
    uint32_t i = (h - OBJ_BASE) / 4;
    if (h < OBJ_BASE || i >= MAX_OBJS || objs[i].kind == O_FREE) return NULL;
    return &objs[i];
}

static void free_obj(uint32_t h) {
    jobj *o = obj(h);
    /* only strings are transient; classes, objects and method IDs stay valid */
    if (o && o->kind == O_STRING) { free(o->str); free(o->sig); memset(o, 0, sizeof *o); }
}

uint32_t rt_jni_new_object(const char *kind) { return new_obj(O_OBJECT, kind, NULL); }
uint32_t rt_jni_new_string(const char *s) { return new_obj(O_STRING, s, NULL); }
uint32_t rt_jni_new_direct_buffer(uint32_t a, uint32_t len) {
    uint32_t h = new_obj(O_DBUF, "dbuf", NULL);
    obj(h)->addr = a;
    obj(h)->len = len;
    return h;
}
const char *rt_jni_string(uint32_t h) { jobj *o = obj(h); return o && o->kind == O_STRING ? o->str : NULL; }
uint32_t rt_jni_env(void) { return JNI_BASE; }

/* ------------------------------------------------------------------ JNI functions */
#define A(i) rt_arg(i)
#define JNI(name) static void j_##name(int idx)

JNI(GetVersion) { rt_ret(0x00010004, 1); }
JNI(FindClass) { rt_ret(new_obj(O_CLASS, rt_guest_str(A(1)), NULL), 2); }
JNI(ExceptionOccurred) { rt_ret(0, 1); }
JNI(ExceptionDescribe) { rt_ret(0, 1); }
JNI(ExceptionClear) { rt_ret(0, 1); }
JNI(ExceptionCheck) { rt_ret(0, 1); }
JNI(PushLocalFrame) { rt_ret(0, 2); }
JNI(PopLocalFrame) { rt_ret(A(1), 2); }
JNI(EnsureLocalCapacity) { rt_ret(0, 2); }
JNI(NewGlobalRef) { rt_ret(A(1), 2); }
JNI(NewLocalRef) { rt_ret(A(1), 2); }
JNI(DeleteGlobalRef) { rt_ret(0, 2); }
JNI(DeleteLocalRef) { free_obj(A(1)); rt_ret(0, 2); }
JNI(IsSameObject) { rt_ret(A(1) == A(2), 3); }
JNI(MonitorEnter) { rt_ret(0, 2); }
JNI(MonitorExit) { rt_ret(0, 2); }
JNI(GetObjectClass) {
    jobj *o = obj(A(1));
    rt_ret(new_obj(O_CLASS, o && o->str ? o->str : "?", NULL), 2);
}
JNI(GetMethodID) { rt_ret(new_obj(O_METHOD, rt_guest_str(A(2)), rt_guest_str(A(3))), 4); }
JNI(GetFieldID) { rt_ret(new_obj(O_FIELD, rt_guest_str(A(2)), rt_guest_str(A(3))), 4); }
JNI(GetIntField) { rt_ret(0, 3); }

JNI(NewStringUTF) { rt_ret(A(1) ? rt_jni_new_string(rt_guest_str(A(1))) : 0, 2); }
JNI(GetStringUTFLength) { const char *s = rt_jni_string(A(1)); rt_ret(s ? (uint32_t)strlen(s) : 0, 2); }
JNI(GetStringUTFChars) {
    const char *s = rt_jni_string(A(1));
    if (A(2)) M[A(2)] = 1;
    rt_ret(rt_strdup_guest(s ? s : ""), 3);
}
JNI(ReleaseStringUTFChars) { rt_heap_free(A(2)); rt_ret(0, 3); }
JNI(GetStringLength) { const char *s = rt_jni_string(A(1)); rt_ret(s ? (uint32_t)strlen(s) : 0, 2); }
JNI(GetStringChars) {
    const char *s = rt_jni_string(A(1));
    if (!s) s = "";
    uint32_t n = (uint32_t)strlen(s), p = rt_heap_alloc(2 * n + 2);
    for (uint32_t i = 0; i <= n; i++) ST16(p + 2 * i, (uint8_t)s[i]);
    if (A(2)) M[A(2)] = 1;
    rt_ret(p, 3);
}
JNI(ReleaseStringChars) { rt_heap_free(A(2)); rt_ret(0, 3); }
JNI(GetStringUTFRegion) {
    const char *s = rt_jni_string(A(1));
    uint32_t start = A(2), len = A(3), buf = A(4);
    if (s) memcpy(M + buf, s + start, len);
    M[buf + len] = 0;
    rt_ret(0, 5);
}
JNI(GetArrayLength) { jobj *o = obj(A(1)); rt_ret(o ? o->len : 0, 2); }
JNI(NewDirectByteBuffer) { rt_ret(rt_jni_new_direct_buffer(A(1), A(2)), 4); }
JNI(GetDirectBufferAddress) { jobj *o = obj(A(1)); rt_ret(o && o->kind == O_DBUF ? o->addr : 0, 2); }
JNI(GetDirectBufferCapacity) { jobj *o = obj(A(1)); cpu.r[EDX] = 0; rt_ret(o && o->kind == O_DBUF ? o->len : 0, 2); }

/* Call<Type>Method[V|A] and CallStatic<Type>Method[V|A].
 * idx encodes the JNI table index; variant = (idx - first) % 3: 0 = varargs (cdecl),
 * 1 = va_list (stdcall, guest pointer), 2 = jvalue array (stdcall). */
static void call_method(int idx) {
    int is_static = idx >= 114;
    int rel = idx - (is_static ? 114 : 34);
    int type = rel / 3, variant = rel % 3;     /* Object Boolean Byte Char Short Int Long Float Double Void */
    uint32_t objh = A(1), mid = A(2);
    jobj *m = obj(mid);
    if (!m || m->kind != O_METHOD) rt_fatal("JNI call with bad method id %08x", mid);
    rt_jvalue args[16];
    int nargs = 0;
    uint32_t p = variant == 0 ? cpu.r[ESP] + 16 : A(3);   /* varargs start after env,obj,mid */
    const char *s = m->sig + 1;
    while (*s && *s != ')' && nargs < 16) {
        char c = *s;
        if (variant == 2) {
            uint32_t slot = p + 8 * (uint32_t)nargs;
            switch (c) {
            case 'Z': case 'B': args[nargs].i = (int8_t)M[slot]; break;
            case 'C': args[nargs].i = LD16(slot); break;
            case 'S': args[nargs].i = (int16_t)LD16(slot); break;
            case 'J': args[nargs].j = (int64_t)LD64(slot); break;
            case 'F': args[nargs].d = F32(LD32(slot)); break;
            case 'D': args[nargs].d = F64(LD64(slot)); break;
            default: args[nargs].i = (int32_t)LD32(slot); break;
            }
        } else if (c == 'J' || c == 'D' || c == 'F') {   /* floats are promoted to double in varargs */
            uint64_t v = LD64(p);
            p += 8;
            if (c == 'J') args[nargs].j = (int64_t)v; else args[nargs].d = F64(v);
        } else {
            args[nargs].i = (int32_t)LD32(p);
            p += 4;
        }
        nargs++;
        while (*s == '[') s++;
        if (*s == 'L') { while (*s && *s != ';') s++; }
        s++;
    }
    rt_jvalue r = rt_java_call(objh, m->str, m->sig, args, nargs);
    int pop = variant == 0 ? 0 : 4;
    if (type == 6) { cpu.r[EDX] = (uint32_t)((uint64_t)r.j >> 32); rt_ret((uint32_t)r.j, pop); }
    else if (type == 7 || type == 8) {
        for (int k = 7; k > 0; k--) RFD(0x1100 + 0x10 * k) = RFD(0x1100 + 0x10 * (k - 1));
        RFD(0x1100) = r.d;
        rt_ret(0, pop);
    } else rt_ret(type == 9 ? 0 : (uint32_t)r.i, pop);
}

static void unimplemented(int idx) { rt_fatal("JNI function #%d not implemented", idx); }

void rt_jni_init(void) {
    rt_mem_commit(JNI_BASE, 0x2000);
    uint32_t table = JNI_BASE + 0x100;
    ST32(JNI_BASE, table);
    static const struct { int idx; const char *name; rt_stub_fn fn; } fns[] = {
#define F(i, n) {i, #n, j_##n}
        F(4, GetVersion), F(6, FindClass), F(15, ExceptionOccurred), F(16, ExceptionDescribe),
        F(17, ExceptionClear), F(19, PushLocalFrame), F(20, PopLocalFrame), F(21, NewGlobalRef),
        F(22, DeleteGlobalRef), F(23, DeleteLocalRef), F(24, IsSameObject), F(25, NewLocalRef),
        F(26, EnsureLocalCapacity), F(31, GetObjectClass), F(33, GetMethodID), F(94, GetFieldID),
        F(100, GetIntField), F(113, GetMethodID), F(144, GetFieldID), F(164, GetStringLength),
        F(165, GetStringChars), F(166, ReleaseStringChars), F(167, NewStringUTF),
        F(168, GetStringUTFLength), F(169, GetStringUTFChars), F(170, ReleaseStringUTFChars),
        F(171, GetArrayLength), F(217, MonitorEnter), F(218, MonitorExit), F(221, GetStringUTFRegion),
        F(228, ExceptionCheck), F(229, NewDirectByteBuffer), F(230, GetDirectBufferAddress),
        F(231, GetDirectBufferCapacity),
#undef F
    };
    static char names[240][40];
    for (int i = 0; i < 240; i++) {
        rt_stub_fn fn = unimplemented;
        const char *name = NULL;
        for (size_t k = 0; k < sizeof fns / sizeof fns[0]; k++) if (fns[k].idx == i) { fn = fns[k].fn; name = fns[k].name; }
        if ((i >= 34 && i <= 63) || (i >= 114 && i <= 143)) { fn = call_method; name = "CallMethod"; }
        snprintf(names[i], sizeof names[i], "JNI_%d_%s", i, name ? name : "missing");
        ST32(table + 4 * (uint32_t)i, rt_stub_register(names[i], fn, i));
    }
}
