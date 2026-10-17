/* Online play for the native host: the engine's SNAP Mobile interface implemented on the
 * Reset Generation Revived relay (server/, docs/MULTIPLAYER.md). Mirrors the Java
 * replacement (src/java/com/redlynx/mt/snapmobile/SNAPMobileClient.java): blocking network
 * actions run on a worker thread, events are delivered to the engine on the game thread. */
#pragma once
#include <stdint.h>

void net_attach(uint8_t *netbuf, int size);      /* the engine's shared packet buffer */
void net_dispatch_events(void);                   /* game thread, before each tick */

/* NativeSNAPMobileInterface methods */
int net_init(void);
void net_start_login(void);
void net_stop_login(void);
int net_login_status(void);
int net_login_cancelled(void);
int net_is_logged_in(void);
int net_login(void);
void net_logout(void);
void net_room(const char *op, const char *name);  /* "create" / "join" / "move" */
void net_leave(const char *name);
int net_send(int len);
void net_friend_list(void);

/* engine callbacks used by net.c (engine.c) */
void rg_ev_simple(const char *name);
void rg_ev_int(const char *name, int v);
void rg_ev_str(const char *name, const char *s);
void rg_ev_data(int len, const char *room);
void rg_ev_friend_list_empty(void);
