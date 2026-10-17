/* Game server mode (see server.h). The engine's server-mode controller (the LAN host screen's,
 * 0x1000ecd0, vtable 0x101779a0) accepts sessions from a "TCP acceptor" (0x100df540) that polls a
 * listener object for MT connections and wraps each one in a session (0x1005d320) it attaches
 * with 0x1000d730. This build has no tcp:// transport, so:
 *   - the acceptor's setController (vtable slot 1) is replaced by a stub that installs a fake
 *     listener object (guest vtable, host stubs);
 *   - the listener's accept() hands out "HostConn" objects: guest objects whose vtable
 *     implements the few MT connection methods the session uses (state, alive, close, send,
 *     recv on channel 0x16);
 *   - messages travel through the relay (docs/MULTIPLAYER.md): the game room bot forwards each
 *     player's channel-0x16 messages (reassembled, bare zlib streams) to this process tagged
 *     with a player id, and fragments ours back into MT frames.
 * The host reaches the LAN host screen through the normal menus (a hook on the menu event
 * handler turns "Arcade Mode / START_QUICK_MATCH" into "host a game"). */
#include "server.h"
#include "host.h"
#include "rt.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define BAD_SOCK INVALID_SOCKET
#define closesock closesocket
#else
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
#define BAD_SOCK (-1)
#define closesock close
#endif

#define MAX_SESS 8
#define CH_GAME 0x16

static int enabled, players = 2;
static uint32_t menu_base;
static void host_game(int players);
#define HOST_AFTER_FRAMES 300      /* the intro is skipped by then (keys) */
static char url[256], room[72];

/* ------------------------------------------------------------------ relay link (WebSocket) */
typedef struct ev { struct ev *next; int kind, pid, len; uint8_t data[]; } ev;
enum { EV_JOIN, EV_LEAVE, EV_DATA, EV_CLOSED };

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER, send_lock = PTHREAD_MUTEX_INITIALIZER;
static ev *ev_head, *ev_tail;
static sock_t ws = BAD_SOCK;
static int ws_up;

static void push_ev(int kind, int pid, const void *data, int len) {
    ev *e = (ev *)calloc(1, sizeof *e + (size_t)(len > 0 ? len : 0));
    e->kind = kind;
    e->pid = pid;
    e->len = len;
    if (len > 0) memcpy(e->data, data, (size_t)len);
    pthread_mutex_lock(&lock);
    if (ev_tail) ev_tail->next = e; else ev_head = e;
    ev_tail = e;
    pthread_mutex_unlock(&lock);
}
static ev *pop_ev(void) {
    pthread_mutex_lock(&lock);
    ev *e = ev_head;
    if (e) { ev_head = e->next; if (!ev_head) ev_tail = NULL; }
    pthread_mutex_unlock(&lock);
    return e;
}

static int send_all(sock_t s, const uint8_t *p, size_t n) {
    while (n) {
        int k = (int)send(s, (const char *)p, (int)n, 0);
        if (k <= 0) return 0;
        p += k;
        n -= (size_t)k;
    }
    return 1;
}
static int recv_all(sock_t s, uint8_t *p, size_t n) {
    while (n) {
        int k = (int)recv(s, (char *)p, (int)n, 0);
        if (k <= 0) return 0;
        p += k;
        n -= (size_t)k;
    }
    return 1;
}

static int ws_send(int opcode, const void *data, size_t len) {
    uint8_t hdr[14], mask[4];
    size_t h = 0;
    uint32_t r = (uint32_t)rand() ^ (uint32_t)time(NULL) * 2654435761u;
    memcpy(mask, &r, 4);
    hdr[h++] = (uint8_t)(0x80 | opcode);
    if (len < 126) hdr[h++] = (uint8_t)(0x80 | len);
    else { hdr[h++] = 0x80 | 126; hdr[h++] = (uint8_t)(len >> 8); hdr[h++] = (uint8_t)len; }
    memcpy(hdr + h, mask, 4);
    h += 4;
    uint8_t *buf = (uint8_t *)malloc(h + len);
    memcpy(buf, hdr, h);
    for (size_t i = 0; i < len; i++) buf[h + i] = ((const uint8_t *)data)[i] ^ mask[i & 3];
    pthread_mutex_lock(&send_lock);
    int ok = ws != BAD_SOCK && send_all(ws, buf, h + len);
    pthread_mutex_unlock(&send_lock);
    free(buf);
    return ok;
}

static int json_str(const char *j, const char *key, char *out, size_t n) {
    char pat[40];
    snprintf(pat, sizeof pat, "\"%s\":\"", key);
    const char *p = strstr(j, pat);
    if (!p) { if (n) out[0] = 0; return 0; }
    p += strlen(pat);
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < n) { if (*p == '\\' && p[1]) p++; out[i++] = *p++; }
    out[i] = 0;
    return 1;
}
static int json_int(const char *j, const char *key, int dflt) {
    char pat[40];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(j, pat);
    return p ? atoi(p + strlen(pat)) : dflt;
}

static void *reader(void *arg) {
    (void)arg;
    uint8_t *msg = (uint8_t *)malloc(1 << 20);
    for (;;) {
        uint8_t h[2];
        if (!recv_all(ws, h, 2)) break;
        int op = h[0] & 15;
        uint64_t len = h[1] & 127;
        if (len == 126) { uint8_t e[2]; if (!recv_all(ws, e, 2)) break; len = (uint64_t)e[0] << 8 | e[1]; }
        else if (len == 127) { uint8_t e[8]; if (!recv_all(ws, e, 8)) break; len = 0; for (int i = 0; i < 8; i++) len = len << 8 | e[i]; }
        if (len >= (1 << 20) - 1) break;
        if (!recv_all(ws, msg, (size_t)len)) break;
        if (op == 1) {
            msg[len] = 0;
            char t[24];
            json_str((char *)msg, "t", t, sizeof t);
            if (!strcmp(t, "player_joined")) push_ev(EV_JOIN, json_int((char *)msg, "pid", -1), NULL, 0);
            else if (!strcmp(t, "player_left")) push_ev(EV_LEAVE, json_int((char *)msg, "pid", -1), NULL, 0);
            else if (!strcmp(t, "error")) host_log("server: relay error %s", (char *)msg);
            else if (getenv("RT_NET_DEBUG")) host_log("server: relay %s", (char *)msg);
        } else if (op == 2) {
            if (len >= 2) push_ev(EV_DATA, msg[0] | msg[1] << 8, msg + 2, (int)len - 2);
        } else if (op == 9) {
            ws_send(10, msg, (size_t)len);
        } else if (op == 8) {
            break;
        }
    }
    free(msg);
    ws_up = 0;
    push_ev(EV_CLOSED, 0, NULL, 0);
    return NULL;
}

static int ws_connect(void) {
    char host[200], path[200] = "/";
    int port = 80;
    if (strncmp(url, "ws://", 5) != 0) { host_log("server: only ws:// relays are supported (got %s)", url); return 0; }
    const char *p = url + 5, *slash = strchr(p, '/'), *colon = strchr(p, ':');
    size_t hl = slash ? (size_t)(slash - p) : strlen(p);
    if (colon && (!slash || colon < slash)) { hl = (size_t)(colon - p); port = atoi(colon + 1); }
    snprintf(host, sizeof host, "%.*s", (int)hl, p);
    if (slash) snprintf(path, sizeof path, "%s", slash);
#ifdef _WIN32
    static int wsa;
    if (!wsa) { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); wsa = 1; }
#endif
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_socktype = SOCK_STREAM;
    char ports[8];
    snprintf(ports, sizeof ports, "%d", port);
    if (getaddrinfo(host, ports, &hints, &res) != 0 || !res) { host_log("server: cannot resolve %s", host); return 0; }
    sock_t s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == BAD_SOCK || connect(s, res->ai_addr, (int)res->ai_addrlen) != 0) {
        host_log("server: cannot connect to %s:%d", host, port);
        if (s != BAD_SOCK) closesock(s);
        freeaddrinfo(res);
        return 0;
    }
    freeaddrinfo(res);
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    char req[600];
    snprintf(req, sizeof req,
             "GET %s HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
             "Sec-WebSocket-Key: UmVzZXRHZW5lcmF0aW9uIQ==\r\nSec-WebSocket-Version: 13\r\n\r\n",
             path, host, port);
    char resp[1024];
    size_t got = 0;
    if (!send_all(s, (const uint8_t *)req, strlen(req))) { closesock(s); return 0; }
    while (got < sizeof resp - 1) {
        int k = (int)recv(s, resp + got, 1, 0);
        if (k <= 0) break;
        got += (size_t)k;
        resp[got] = 0;
        if (got >= 4 && !memcmp(resp + got - 4, "\r\n\r\n", 4)) break;
    }
    if (!strstr(resp, " 101 ")) { host_log("server: websocket upgrade refused"); closesock(s); return 0; }
    ws = s;
    ws_up = 1;
    pthread_t t;
    pthread_create(&t, NULL, reader, NULL);
    pthread_detach(t);
    char json[200];
    snprintf(json, sizeof json, "{\"t\":\"hello\",\"v\":1,\"name\":\"RGServer\"}");
    ws_send(1, json, strlen(json));
    snprintf(json, sizeof json, "{\"t\":\"serve\",\"room\":\"%s\"}", room);
    ws_send(1, json, strlen(json));
    host_log("server: serving room %s on %s", room, url);
    return 1;
}

/* ------------------------------------------------------------------ sessions */
typedef struct { int used, pid, accepted, closed; uint32_t obj, msg, addr; ev *in_head, *in_tail; } sess_t;
static sess_t sess[MAX_SESS];

static sess_t *by_pid(int pid) {
    for (int i = 0; i < MAX_SESS; i++) if (sess[i].used && sess[i].pid == pid) return &sess[i];
    return NULL;
}

void rg_server_pump(void) {
    if (!enabled) return;
    static int frames, hosted;
    if (!hosted && menu_base && ++frames > HOST_AFTER_FRAMES) { hosted = 1; host_game(players); }
    static uint32_t retry;
    if (!ws_up && ws == BAD_SOCK) {
        uint32_t now = host_time_ms();
        if (now - retry > 2000 || !retry) { retry = now ? now : 1; ws_connect(); }
    }
    static uint32_t last_ping;
    if (ws_up && host_time_ms() - last_ping > 15000) { last_ping = host_time_ms(); ws_send(1, "{\"t\":\"ping\"}", 12); }
    ev *e;
    while ((e = pop_ev())) {
        sess_t *s = by_pid(e->pid);
        if (e->kind == EV_JOIN) {
            if (!s) {
                for (int i = 0; i < MAX_SESS && !s; i++) if (!sess[i].used) s = &sess[i];
                if (s) { memset(s, 0, sizeof *s); s->used = 1; s->pid = e->pid; host_log("server: player %d joined", e->pid); }
            }
            free(e);
        } else if (e->kind == EV_LEAVE) {
            if (s) { s->closed = 1; host_log("server: player %d left", e->pid); }
            free(e);
        } else if (e->kind == EV_DATA && s) {
            e->next = NULL;
            if (s->in_tail) s->in_tail->next = e; else s->in_head = e;
            s->in_tail = e;
        } else if (e->kind == EV_CLOSED) {
            host_log("server: relay connection lost");
            if (ws != BAD_SOCK) { closesock(ws); ws = BAD_SOCK; }
            for (int i = 0; i < MAX_SESS; i++) if (sess[i].used) sess[i].closed = 1;
            free(e);
        } else free(e);
    }
}

static void to_player(sess_t *s, const uint8_t *data, int len) {
    uint8_t *buf = (uint8_t *)malloc((size_t)len + 2);
    buf[0] = (uint8_t)s->pid;
    buf[1] = (uint8_t)(s->pid >> 8);
    memcpy(buf + 2, data, (size_t)len);
    ws_send(2, buf, (size_t)len + 2);
    free(buf);
}

/* ------------------------------------------------------------------ guest objects */
static uint32_t listener_obj, conn_vt;

static void bad_slot(int slot) {
    rt_fatal("server: unimplemented MT object method (vtable slot 0x%x, this=%08x, ret=%08x)", slot, cpu.r[ECX], LD32(cpu.r[ESP]));
}

static uint32_t make_vtable(const char *name, int slots) {
    uint32_t vt = rt_heap_alloc((uint32_t)slots * 4);
    for (int i = 0; i < slots; i++) {
        char *n = (char *)malloc(64);
        snprintf(n, 64, "%s+0x%x", name, i * 4);
        ST32(vt + (uint32_t)i * 4, rt_stub_register(n, bad_slot, i * 4));
    }
    return vt;
}

static sess_t *sess_of(uint32_t obj) { int i = (int)LD32(obj + 4); return i >= 0 && i < MAX_SESS ? &sess[i] : NULL; }

/* acceptor (0x100df500): setController(controller) */
static void acc_set_controller(int arg) {
    (void)arg;
    uint32_t acc = cpu.r[ECX], ctrl = rt_arg(0);
    ST32(acc + 4, ctrl);
    ST32(acc + 8, listener_obj);
    ST8(acc + 0x18, 1);
    host_log("server: controller %08x attached to the acceptor %08x", ctrl, acc);
    rt_ret(0, 1);
}

/* listener: accept() -> connection or 0 */
static void l_accept(int arg) {
    (void)arg;
    for (int i = 0; i < MAX_SESS; i++) {
        sess_t *s = &sess[i];
        if (s->used && !s->accepted) {
            s->accepted = 1;
            s->obj = rt_heap_alloc(0x400);
            memset(M + s->obj, 0, 0x400);
            ST32(s->obj, conn_vt);
            ST32(s->obj + 4, (uint32_t)i);
            host_log("server: accepted player %d (conn %08x)", s->pid, s->obj);
            rt_ret(s->obj, 0);
            return;
        }
    }
    rt_ret(0, 0);
}
static void l_close(int arg) { (void)arg; rt_ret(0, 0); }
static void any_dtor(int arg) { (void)arg; rt_ret(cpu.r[ECX], 1); }

/* connection */
/* conn.address(): an address object; callers only print its string (0x10111c60 -> 0x100ee800):
 * vtable slot 0x14 (no-op), string object at +0xc -> header -> char* at +0xc. */
static uint32_t addr_vt;
static void addr_nop(int arg) { (void)arg; rt_ret(0, 1); }
static void c_address(int arg) {
    (void)arg;
    sess_t *s = sess_of(cpu.r[ECX]);
    if (s && !s->addr) {
        char name[32];
        snprintf(name, sizeof name, "relay:player%d", s->pid);
        s->addr = rt_heap_alloc(0x20);
        uint32_t hdr = rt_heap_alloc(0x20);
        memset(M + s->addr, 0, 0x20);
        memset(M + hdr, 0, 0x20);
        ST32(hdr + 0xc, rt_strdup_guest(name));
        ST32(s->addr, addr_vt);
        ST32(s->addr + 0xc, hdr);
    }
    rt_ret(s ? s->addr : 0, 0);
}
static void c_state(int arg) { (void)arg; rt_ret(3, 0); }
static void c_alive(int arg) { (void)arg; sess_t *s = sess_of(cpu.r[ECX]); rt_ret(s && !s->closed, 0); }
static void c_close(int arg) { (void)arg; sess_t *s = sess_of(cpu.r[ECX]); if (s) s->closed = 1; rt_ret(0, 0); }
static void c_send(int arg) {
    (void)arg;
    sess_t *s = sess_of(cpu.r[ECX]);
    uint32_t ch = rt_arg(0), ptr = rt_arg(1), len = rt_arg(2);
    if (s && ch == CH_GAME && len <= 1u << 20) {
        if (getenv("RT_NET_DEBUG")) host_log("server: -> player %d: %u bytes", s->pid, len);
        to_player(s, M + ptr, (int)len);
    }
    rt_ret(1, 3);
}
/* recv(channel, flag) -> message object or 0 (MT message: ByteStream at +0xc) */
static void c_recv(int arg) {
    (void)arg;
    uint32_t obj = cpu.r[ECX], ch = rt_arg(0);
    sess_t *s = sess_of(obj);
    if (!s || ch != CH_GAME || !s->in_head) { rt_ret(0, 2); return; }
    ev *e = s->in_head;
    s->in_head = e->next;
    if (!s->in_head) s->in_tail = NULL;
    /* messages are never freed: the engine may keep them (the real connection pools them) */
    uint32_t msg = rt_heap_alloc(0x48), cap[1] = {(uint32_t)e->len + 0x40};
    memset(M + msg, 0, 0x48);
    rt_guest_thiscall(0x1011f700, msg, 1, cap);     /* MT message constructor */
    uint32_t buf = rt_heap_alloc((uint32_t)e->len);
    memcpy(M + buf, e->data, (size_t)e->len);
    uint32_t stream = msg + 0xc, w[2] = {buf, (uint32_t)e->len};
    rt_guest_thiscall(LD32(LD32(stream + 8)), stream + 8, 2, w);   /* ostream part (+8) write(buf, len): 0x100f3b90 */
    rt_heap_free(buf);
    if (getenv("RT_NET_DEBUG")) host_log("server: <- player %d: %d bytes", s->pid, e->len);
    free(e);
    s->msg = msg;
    rt_ret(msg, 2);
}

/* ------------------------------------------------------------------ menu hook */
/* menu_base: the main menu flow object (constructor 0x1008f500) */
static void hook_menu_ctor(void) { menu_base = cpu.r[ECX]; }
static const char *ev_name(uint32_t ev) { uint32_t hdr = LD32(ev + 4); return (const char *)(M + LD32(hdr + 12)); }
static void hook_menu(void) {
    if (!getenv("RT_NET_DEBUG")) return;
    uint32_t ev = LD32(cpu.r[ESP] + 4);
    host_log("server: menu event type %u \"%s\" (this=%08x)", LD32(ev), ev_name(ev), cpu.r[ECX]);
}

/* What the menus do for "host a game" (START_QUICK_MATCH with the hosting flag, handler
 * 0x1008fa80): build the LAN host screen (0x100caa80: a hero list whose length selects the
 * mission, so it also fixes the player count) and make it the current screen. Its entry
 * (0x100ca920) creates the server-mode controller and the acceptor. */
static void host_game(int players) {
    uint32_t ctx = LD32(menu_base + 0x10);
    uint32_t list = rt_heap_alloc(0xc), val = rt_heap_alloc(4);
    memset(M + list, 0, 0xc);
    for (int i = 0; i < players; i++) {
        uint32_t a[1] = {val};
        ST32(val, (uint32_t)i);
        rt_guest_thiscall(0x10104a40, list, 1, a);          /* list.push_back(&hero) */
    }
    uint32_t screen = rt_heap_alloc(0x3c), a2[2] = {ctx, list};
    memset(M + screen, 0, 0x3c);
    rt_guest_thiscall(0x100caa80, screen, 2, a2);           /* LAN host screen */
    uint32_t a3[1] = {screen};
    rt_guest_thiscall(LD32(LD32(menu_base) + 0x14), menu_base, 1, a3);   /* make it the current screen */
    host_log("server: host screen %08x for %d players", screen, players);
}

void rg_server_enable(const char *u, const char *r) {
    enabled = 1;
    snprintf(url, sizeof url, "%s", u);
    snprintf(room, sizeof room, "%s", r);
    rt_hook_add(0x1008fa80, hook_menu);
    rt_hook_add(0x1008f500, hook_menu_ctor);
}
int rg_server_enabled(void) { return enabled; }

void rg_server_init(void) {
    if (!enabled) return;
    uint32_t lvt = make_vtable("listener", 32);
    ST32(lvt + 0x00, rt_stub_register("listener.dtor", any_dtor, 0));
    ST32(lvt + 0x4c, rt_stub_register("listener.accept", l_accept, 0));
    ST32(lvt + 0x5c, rt_stub_register("listener.close", l_close, 0));
    listener_obj = rt_heap_alloc(0x20);
    memset(M + listener_obj, 0, 0x20);
    ST32(listener_obj, lvt);
    conn_vt = make_vtable("conn", 40);
    ST32(conn_vt + 0x00, rt_stub_register("conn.dtor", any_dtor, 0));
    addr_vt = make_vtable("addr", 24);
    ST32(addr_vt + 0x14, rt_stub_register("addr.nop", addr_nop, 0));
    ST32(conn_vt + 0x14, rt_stub_register("conn.address", c_address, 0));
    ST32(conn_vt + 0x18, rt_stub_register("conn.state", c_state, 0));
    ST32(conn_vt + 0x2c, rt_stub_register("conn.alive", c_alive, 0));
    ST32(conn_vt + 0x5c, rt_stub_register("conn.close", c_close, 0));
    ST32(conn_vt + 0x68, rt_stub_register("conn.send", c_send, 0));
    ST32(conn_vt + 0x7c, rt_stub_register("conn.recv", c_recv, 0));
    /* TCP acceptor vtable 0x10188b64: slot 1 = setController */
    ST32(0x10188b68, rt_stub_register("acceptor.setController", acc_set_controller, 0));
    host_log("server: game server mode, relay %s room %s", url, room);
}
