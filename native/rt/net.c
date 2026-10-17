/* Relay client (WebSocket, ws:// only) and the SNAP Mobile login/room state machine. */
#include "net.h"
#include "engine.h"
#include "host.h"
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
#include <sys/time.h>
#include <unistd.h>
typedef int sock_t;
#define BAD_SOCK (-1)
#define closesock close
#endif

#define REQUEST_TIMEOUT_MS 10000
#define MAX_PACKET 2048

static char server_url[256] = "ws://127.0.0.1:8787/ws";
static char nickname[32] = "";
static uint8_t *netbuf;
static int netbuf_size;

void rg_net_configure(const char *url, const char *nick) {
    if (url) snprintf(server_url, sizeof server_url, "%s", url);
    if (nick) snprintf(nickname, sizeof nickname, "%s", nick);
}
void net_attach(uint8_t *buf, int size) { netbuf = buf; netbuf_size = size; }

/* ------------------------------------------------------------------ queues */
enum { A_LOGIN, A_LOGOUT, A_CREATE, A_JOIN, A_MOVE, A_LEAVE, A_SEND, A_FRIENDS };
enum { E_LOGIN_DONE, E_LOGIN_CANCELLED, E_CONNECTED, E_LEFT_ROOM, E_NAF, E_DATA, E_JOIN_RETRY, E_FRIENDS };
typedef struct item { struct item *next; int kind, arg, len; char name[72]; uint8_t data[]; } item;
typedef struct { item *head, *tail; } queue;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t action_cv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t reply_cv = PTHREAD_COND_INITIALIZER;
static queue actions, events;
static int running;

static item *mk(int kind, int arg, const char *name, const void *data, int len) {
    item *it = (item *)calloc(1, sizeof *it + (size_t)(len > 0 ? len : 0));
    it->kind = kind;
    it->arg = arg;
    it->len = len;
    if (name) snprintf(it->name, sizeof it->name, "%s", name);
    if (len > 0) memcpy(it->data, data, (size_t)len);
    return it;
}
static void push(queue *q, item *it) { if (q->tail) q->tail->next = it; else q->head = it; q->tail = it; }
static item *pop(queue *q) { item *it = q->head; if (it) { q->head = it->next; if (!q->head) q->tail = NULL; } return it; }

/* state (guarded by lock) */
static int logged_in, cancelled, logout_queued, login_in_progress, err_state, credentials_ok;
static int in_room, join_retries;
static char room[72];

static void add_event(item *it) { pthread_mutex_lock(&lock); push(&events, it); pthread_mutex_unlock(&lock); }

static void add_action(item *it, int allow_logged_out) {
    pthread_mutex_lock(&lock);
    if (!allow_logged_out && (logout_queued || !logged_in)) { pthread_mutex_unlock(&lock); free(it); return; }
    if (it->kind == A_LOGOUT) logout_queued = 1;
    push(&actions, it);
    pthread_cond_signal(&action_cv);
    pthread_mutex_unlock(&lock);
}

/* ------------------------------------------------------------------ WebSocket */
static sock_t ws = BAD_SOCK;
static pthread_mutex_t send_lock = PTHREAD_MUTEX_INITIALIZER;
static char reply[512];          /* last control reply (JSON) */
static int reply_seq;
static int ws_generation;

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
    uint8_t hdr[14];
    size_t h = 0;
    uint8_t mask[4];
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

/* tiny JSON field reader for the relay's flat messages */
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

static void on_disconnect(int gen) {
    pthread_mutex_lock(&lock);
    int was = logged_in && gen == ws_generation && !logout_queued;
    pthread_mutex_unlock(&lock);
    if (was) {
        host_log("net: relay connection lost");
        pthread_mutex_lock(&lock);
        item *it;
        while ((it = pop(&actions))) free(it);
        pthread_mutex_unlock(&lock);
        add_event(mk(E_NAF, 0, NULL, NULL, 0));
        add_action(mk(A_LOGOUT, 0, NULL, NULL, 0), 1);
    }
}

static void *reader(void *arg) {
    int gen = (int)(intptr_t)arg;
    sock_t s = ws;
    uint8_t *msg = (uint8_t *)malloc(65536);
    for (;;) {
        uint8_t h[2];
        if (!recv_all(s, h, 2)) break;
        int op = h[0] & 15;
        uint64_t len = h[1] & 127;
        if (len == 126) { uint8_t e[2]; if (!recv_all(s, e, 2)) break; len = (uint64_t)e[0] << 8 | e[1]; }
        else if (len == 127) { uint8_t e[8]; if (!recv_all(s, e, 8)) break; len = 0; for (int i = 0; i < 8; i++) len = len << 8 | e[i]; }
        if (len > 65535) break;
        if (!recv_all(s, msg, (size_t)len)) break;
        if (op == 1) {                                   /* text: control reply / notice */
            msg[len] = 0;
            char t[24];
            json_str((char *)msg, "t", t, sizeof t);
            if (!strcmp(t, "pong") || !strcmp(t, "peer_joined") || !strcmp(t, "peer_left")) continue;
            pthread_mutex_lock(&lock);
            snprintf(reply, sizeof reply, "%s", (char *)msg);
            reply_seq++;
            pthread_cond_broadcast(&reply_cv);
            pthread_mutex_unlock(&lock);
        } else if (op == 2) {                            /* binary: game packet */
            pthread_mutex_lock(&lock);
            int ok = in_room && len <= MAX_PACKET;
            pthread_mutex_unlock(&lock);
            if (ok) add_event(mk(E_DATA, 0, room, msg, (int)len));
        } else if (op == 9) {
            ws_send(10, msg, (size_t)len);
        } else if (op == 8) {
            break;
        }
    }
    free(msg);
    on_disconnect(gen);
    return NULL;
}

/* sends a control message and waits for the reply of `type` (or an error); 1 on `type` */
static int request(const char *json, const char *type, char *out, size_t n) {
    pthread_mutex_lock(&lock);
    int seq = reply_seq;
    pthread_mutex_unlock(&lock);
    if (!ws_send(1, json, strlen(json))) return 0;
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += REQUEST_TIMEOUT_MS / 1000;
    pthread_mutex_lock(&lock);
    int got = 0;
    while (!got) {
        while (reply_seq == seq) {
            if (pthread_cond_timedwait(&reply_cv, &lock, &until) != 0) { pthread_mutex_unlock(&lock); return 0; }
        }
        seq = reply_seq;
        char t[24];
        json_str(reply, "t", t, sizeof t);
        if (!strcmp(t, type) || !strcmp(t, "error")) {
            got = !strcmp(t, type) ? 1 : -1;
            if (out) snprintf(out, n, "%s", reply);
        }
    }
    pthread_mutex_unlock(&lock);
    return got == 1;
}

static void ws_close(void) {
    pthread_mutex_lock(&send_lock);
    if (ws != BAD_SOCK) {
        uint8_t c[6] = {0x88, 0x80, 0, 0, 0, 0};
        send_all(ws, c, 6);
        shutdown(ws, 2);
        closesock(ws);
        ws = BAD_SOCK;
    }
    pthread_mutex_unlock(&send_lock);
}

static int ws_connect(void) {
    char host[200], path[200] = "/";
    int port = 80;
    if (strncmp(server_url, "ws://", 5) != 0) {
        host_log("net: only ws:// servers are supported by the native client (got %s)", server_url);
        return 0;
    }
    const char *p = server_url + 5, *slash = strchr(p, '/'), *colon = strchr(p, ':');
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
    if (getaddrinfo(host, ports, &hints, &res) != 0 || !res) { host_log("net: cannot resolve %s", host); return 0; }
    sock_t s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == BAD_SOCK || connect(s, res->ai_addr, (int)res->ai_addrlen) != 0) {
        host_log("net: cannot connect to %s:%d", host, port);
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
    if (!strstr(resp, " 101 ")) { host_log("net: websocket upgrade refused"); closesock(s); return 0; }
    ws = s;
    pthread_mutex_lock(&lock);
    int gen = ++ws_generation;
    pthread_mutex_unlock(&lock);
    pthread_t t;
    pthread_create(&t, NULL, reader, (void *)(intptr_t)gen);
    pthread_detach(t);
    return 1;
}

/* ------------------------------------------------------------------ worker */
static int enter_room(const char *op, const char *name) {
    char json[160], out[512];
    if (in_room && strcmp(room, name) != 0) {
        request("{\"t\":\"leave\"}", "left", NULL, 0);
        add_event(mk(E_LEFT_ROOM, 0, room, NULL, 0));
        in_room = 0;
    }
    snprintf(json, sizeof json, "{\"t\":\"%s\",\"room\":\"%s\"}", op, name);
    if (request(json, "joined", out, sizeof out)) {
        pthread_mutex_lock(&lock);
        snprintf(room, sizeof room, "%s", name);
        in_room = 1;
        pthread_mutex_unlock(&lock);
        add_event(mk(E_CONNECTED, 0, NULL, NULL, 0));
        return 1;
    }
    char code[32];
    json_str(out, "code", code, sizeof code);
    host_log("net: %s(%s) failed: %s", op, name, code[0] ? code : "timeout");
    in_room = 0;
    return 0;
}

static void do_login(void) {
    login_in_progress = 1;
    if (logged_in && ws != BAD_SOCK) { credentials_ok = 1; add_event(mk(E_LOGIN_DONE, 0, NULL, NULL, 0)); login_in_progress = 0; return; }
    if (!nickname[0]) snprintf(nickname, sizeof nickname, "Player%04d", rand() % 9000 + 1000);
    if (!ws_connect()) { err_state = 3; login_in_progress = 0; return; }
    char json[120], out[512];
    snprintf(json, sizeof json, "{\"t\":\"hello\",\"v\":1,\"name\":\"%s\"}", nickname);
    if (!request(json, "welcome", out, sizeof out)) {
        host_log("net: login rejected: %s", out);
        ws_close();
        err_state = 2;
        login_in_progress = 0;
        return;
    }
    char name[32];
    json_str(out, "name", name, sizeof name);
    host_log("net: logged in to %s as %s", server_url, name);
    pthread_mutex_lock(&lock);
    logged_in = 1;
    credentials_ok = 1;
    pthread_mutex_unlock(&lock);
    add_event(mk(E_LOGIN_DONE, 0, NULL, NULL, 0));
    login_in_progress = 0;
}

static void do_logout(void) {
    pthread_mutex_lock(&lock);
    logged_in = 0;
    in_room = 0;
    credentials_ok = 0;
    pthread_mutex_unlock(&lock);
    ws_close();
    add_event(mk(E_LOGIN_CANCELLED, 0, NULL, NULL, 0));
    pthread_mutex_lock(&lock);
    logout_queued = 0;
    pthread_mutex_unlock(&lock);
}

static void *worker(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&lock);
        while (!actions.head) pthread_cond_wait(&action_cv, &lock);
        item *it = pop(&actions);
        pthread_mutex_unlock(&lock);
        switch (it->kind) {
        case A_LOGIN: do_login(); break;
        case A_LOGOUT: do_logout(); break;
        case A_CREATE: enter_room("create", it->name); break;
        case A_MOVE: enter_room("move", it->name); break;
        case A_JOIN:
            if (!enter_room("join", it->name)) {
                if (join_retries < 4) {
#ifdef _WIN32
                    Sleep(3000);
#else
                    sleep(3);
#endif
                    join_retries++;
                    add_event(mk(E_JOIN_RETRY, 0, it->name, NULL, 0));
                } else {
                    err_state = 3;
                    add_event(mk(E_NAF, 2, NULL, NULL, 0));
                    add_action(mk(A_LOGOUT, 0, NULL, NULL, 0), 1);
                }
            }
            break;
        case A_LEAVE:
            if (in_room && !strcmp(room, it->name)) { request("{\"t\":\"leave\"}", "left", NULL, 0); in_room = 0; }
            add_event(mk(E_LEFT_ROOM, 0, it->name, NULL, 0));
            break;
        case A_SEND:
            if (!ws_send(2, it->data, (size_t)it->len)) on_disconnect(ws_generation);
            break;
        case A_FRIENDS: add_event(mk(E_FRIENDS, 0, NULL, NULL, 0)); break;
        }
        free(it);
    }
    return NULL;
}

static void *pinger(void *arg) {
    (void)arg;
    for (;;) {
#ifdef _WIN32
        Sleep(15000);
#else
        sleep(15);
#endif
        if (logged_in) ws_send(1, "{\"t\":\"ping\"}", 12);
    }
    return NULL;
}

/* ------------------------------------------------------------------ engine-facing */
int net_init(void) {
    if (!running) {
        running = 1;
        pthread_t t;
        pthread_create(&t, NULL, worker, NULL);
        pthread_detach(t);
        pthread_create(&t, NULL, pinger, NULL);
        pthread_detach(t);
    }
    return 1;
}

void net_start_login(void) { err_state = 0; credentials_ok = 0; cancelled = 0; }
void net_stop_login(void) {}
int net_login_status(void) {
    if (err_state == 3) return 7;
    if (err_state == 1) return 6;
    if (err_state == 2) return 5;
    if (cancelled && login_in_progress) return 1;
    if (cancelled) return 2;
    if (credentials_ok) return 4;
    return 3;
}
int net_login_cancelled(void) { return cancelled; }
int net_is_logged_in(void) { return logged_in; }
int net_login(void) { add_action(mk(A_LOGIN, 0, NULL, NULL, 0), 1); return 1; }
void net_logout(void) { add_action(mk(A_LOGOUT, 0, NULL, NULL, 0), 1); cancelled = 1; }
void net_room(const char *op, const char *name) {
    if (!name) return;
    if (!strcmp(op, "join")) join_retries = 0;
    add_action(mk(!strcmp(op, "create") ? A_CREATE : !strcmp(op, "join") ? A_JOIN : A_MOVE, 0, name, NULL, 0), 0);
}
void net_leave(const char *name) { add_action(mk(A_LEAVE, 0, name ? name : "", NULL, 0), 0); }
int net_send(int len) {
    if (len > 0 && len <= netbuf_size) add_action(mk(A_SEND, 0, NULL, netbuf, len), 0);
    return len;
}
void net_friend_list(void) { add_action(mk(A_FRIENDS, 0, NULL, NULL, 0), 0); }

void net_dispatch_events(void) {
    for (;;) {
        pthread_mutex_lock(&lock);
        item *it = pop(&events);
        pthread_mutex_unlock(&lock);
        if (!it) return;
        switch (it->kind) {
        case E_LOGIN_DONE: rg_ev_simple("onArenaLoginCompleted"); break;
        case E_LOGIN_CANCELLED: rg_ev_simple("onArenaLoginCancelled"); break;
        case E_CONNECTED: rg_ev_simple("onConnected"); break;
        case E_LEFT_ROOM: rg_ev_str("onDisconnectedFromGameRoom", it->name); break;
        case E_NAF: rg_ev_int("onDisconnectedFromNAF", it->arg); break;
        case E_DATA:
            if (netbuf && it->len <= netbuf_size) {
                memcpy(netbuf, it->data, (size_t)it->len);
                if (getenv("RT_NET_DEBUG")) host_log("net: data %d bytes for room \"%s\"", it->len, it->name);
                rg_ev_data(it->len, it->name);
            }
            break;
        case E_JOIN_RETRY: add_action(mk(A_JOIN, 0, it->name, NULL, 0), 0); break;
        case E_FRIENDS: rg_ev_friend_list_empty(); break;
        }
        free(it);
    }
}
