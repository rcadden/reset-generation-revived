/* Game server mode: runs the engine's own server-mode controller (the LAN host screen's)
 * with network players that arrive through the relay instead of tcp:// sockets.
 * See docs/MULTIPLAYER.md, "Next step: game server on the host player's device". */
#pragma once

/* Call before rg_init. url = relay (ws://host:port/ws), room = the game room to serve
 * (e.g. "QM_1"). The host then drives the menus to the LAN host screen (see rgtest --server). */
void rg_server_enable(const char *url, const char *room);
int rg_server_enabled(void);

/* engine glue (engine.c / rt) */
void rg_server_init(void);          /* after the engine image is loaded */
void rg_server_pump(void);          /* game thread, before each tick */
