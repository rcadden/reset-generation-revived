# Multiplayer: our own server for Quick Match / Online

Status (2026-10-17):

| Piece | State |
|---|---|
| Relay server (`server/`, Node + Cloudflare Worker) | **Done**, 15 tests (`npm test`) |
| Client: Windows (Java replacement for `SNAPMobileClient`) | **Done** — logs in to the relay with a nickname, no Arena login screen |
| Client: native/Android (`native/rt/net.c`) | **Done** (plain `ws://` only) |
| Casino system (login, casino enter, Quick Match queue, lock, matched-up) | **Done** — both players listed in "Players in Match", "Game starting…" |
| Game room bot (`server/src/gameroom.js`: handshake, seating on 0x15, forwarding) | **Done** — both players seated in a shared room |
| Game server (channel 0x16: world setup, then the match) | **Open** — the engine needs its own server-mode controller running somewhere; plan in "Next step" |
| Friendly arenas, rankings, statistics, achievements, friends | Answered with empty/OK replies so the client does not stall; not implemented |

## How online worked in 2008–2010

Nokia **SNAP Mobile** (N-Gage Arena) provided accounts, named game rooms and per-room "game
servers". RedLynx's Java glue (`com.redlynx.mt.snapmobile`) exposes rooms and packets to the
native engine; everything above that — the engine's **MT** networking layer, the casino
(matchmaking) protocol, seating, and the match itself — is in the engine DLL.

```
engine MT layer ──JNI──► SNAPMobileClient (Java)  ──►  SNAP server: rooms, game servers
                         replaced by: Java client / native net.c ──WebSocket──► our relay
```

### Engine-facing contract (unchanged)

Engine → host (`NativeSNAPMobileInterface`): `init`, `startLoginScreen`, `getLoginScreenStatus`,
`login`, `logout`, `isLoggedIn`, `createRoom`, `joinRoom`, `moveToRoom`, `leaveRoom`,
`send(len)` (bytes in the shared 2 KB buffer), friends/presence calls.
Host → engine (`NativeSNAPMobileEventListener`): `onArenaLoginCompleted/Cancelled`,
`onConnected` (joined a room), `onDataReceived(len, room)`, `onDisconnectedFromGameRoom`,
`onDisconnectedFromNAF`, `onFriendList`, ….

## Relay protocol (ours)

WebSocket at `/ws`. Text frames are JSON control messages, binary frames are game packets.

| Client → server | Server → client |
|---|---|
| `{"t":"hello","v":1,"name":"Nick"}` | `{"t":"welcome","id","name"}` (name made unique) |
| `{"t":"create"\|"join"\|"move","room":"X"}` | `{"t":"joined","room","members":[…]}` or `{"t":"error","code":"not_found"\|"full"…}` |
| `{"t":"leave"}` | `{"t":"left"}`; others get `peer_joined` / `peer_left` |
| `{"t":"ping"}` | `{"t":"pong"}` |
| `{"t":"rooms"}` | `{"t":"rooms","rooms":[…]}` |
| binary packet (≤ 2048 bytes) | relayed in order to the other room members — except in `CasinoSystem` |

Rooms hold 4 players; `CasinoSystem` is a permanent lobby (1,000) whose packets go to the
casino bot instead of other players. The bot behaves like an MT peer: it opens with a
transport hello (channel 2 `00`) and the keep-alive frame, accepts the client's hello (`20`),
answers its pings and pings every 10 s. Without the hello the client never starts its own
keep-alive and drops the connection ("Connection lost") 20 s after entering the casino.

**Room greeting.** The engine only starts talking in a room after the room's server has
spoken. Plain rooms: the relay sends each joiner one frame (`05 00 01 20 4e 00 00`).
Matchmade rooms get a `GameRoom` bot that greets like the casino bot (channel 2 `00` +
keep-alive) once all matched players are present (max 10 s), because the engine's
handshake packets are lost on players who have not joined yet.

## The engine's wire format (reconstructed)

Inside a game packet: one or more frames `[u16 length][u8 channel][payload]`, little-endian.

| Channel | Use |
|---|---|
| 1 | keep-alive: `u32 20000` on join (peer timeout, ms), then 1-byte sequence pings |
| 2 | transport control: `00` = hello on join, `20` = accept, 1-byte ping replies (same sequence) |
| 3 | key/value "command": session hello (`name`, `pid`, `passwd`, `roomname` = room URL, …) |
| 0x11 | casino-system messages |
| 0x12 / 0x13 | messages to a matchmaker / friendly casino |
| 0x15 | seating in a game room (`SEAT_RESERVE`/`SEAT_TAKE`/`SEAT_LEAVE` + `u32` player id) |
| 0x16 | the game protocol: client ↔ authoritative game server (see below) |

Casino messages: `[u8 type][u8 action]` + fields; type 0 REQUEST, 1 RESPONSE, 2 EVENT; the
42 actions are in `server/src/casino.js` (`ACTIONS`). Strings are the engine's string object
`[u32 refcount|flags][u32 length][u32 capacity][bytes]` (flags bits 28–30 = 2: UTF-16).
Key/value maps: `u32 count` + key/value string pairs. A generic RESPONSE is `[u8 result
(0 = OK)][u32 value]`.

Messages the bot handles (the engine's deserializers are the reference; addresses are in the
engine DLL):

| Message | Layout |
|---|---|
| REQUEST `CASINOSYSTEM_LOGIN` | `u32 1`, string `"Reset Generation v. 1.1.3"` |
| RESPONSE login | generic + profile map + `u8 1` + casino descriptor |
| profile map | `id`, `name`, `fameLevel`, `heroType`, `random` (the client crashes on a missing key) |
| casino descriptor | `u8 kind` (1 matchmaker, 2 friendly), `u32 id`, 3 × `u32`, string name |
| REQUEST `CASINO_ENTER` (ch 0x12) | `u32 casino id` → RESPONSE generic + profile map + casino descriptor |
| REQUEST `CASINO_MATCHMAKING` | string hero (`PLUMBER`…), string random (`FALSE`) → RESPONSE generic + `u8 n` + profile maps of the players already waiting |
| EVENT `CASINO_ENTER` / `CASINO_MATCHMAKING` (8 / 9) | string id + profile map. 9 adds a player to the waiting screen's list (the screen exists while queued); 8 registers it in the client's casino, where the seating events look ids up |
| EVENT `CASINO_CANCEL_MATCHMAKING` (10) | string id: removes a player from the list |
| REQUEST `LOCK_PLAYER_IN_GAME` (29, ch 0x11) | `u32 0` → "Game starting…"; the client answers RESPONSE 29. `UNLOCK_PLAYER_FROM_GAME` (30) shows "matchmaking failed" |
| EVENT `CASINO_MATCHED_UP` | string casino name, table: string room, string address, `u8`, `u8`, `u32` players, settings (`u8 0`, `u8`, string address, `u8`) |

The address is an MT URL: `naf://rg.com:80/QM_1?hosted=yes`. The path is the SNAP room the
client moves to; `hosted` decides whether it creates or joins it. (A `room=` query parameter
breaks the client's room check — leave it out.) The login and casino flow also includes
`GET_STATISTICS`, `GET_ACHIEVEMENTS`, `REGISTER_FRIENDS` and `GET_FRIENDLY_CASINO_LIST`,
currently answered with empty OK responses.

## Running a server

```bash
cd server
npm install
npm start                       # ws://0.0.0.0:8787/ws   (--log-packets to hex-dump traffic)
npm test
npx wrangler login && npx wrangler deploy     # Cloudflare (Durable Object), wss://…workers.dev/ws
```

Clients: Windows `app\revived.ini` → `server=ws://<host>:8787/ws`, `nickname=…`.
Android: `files/revived.ini` in the app's storage (or the APK's built-in default,
`tools/build_android.sh SERVER=ws://…`). The native client speaks `ws://` only for now, so
Android needs a self-hosted server (LAN or a VPS) until TLS is added; the Java client supports
`wss://` (Cloudflare).

## The game room (done)

Quick Match end to end, as served by `casino.js` and `gameroom.js` (verified with two
`rgtest` clients):

1. `CASINO_MATCHMAKING` → the "Players in Match" screen opens while the player waits; it lists
   the players from the RESPONSE and from later EVENTs 9 (not the seating events).
2. Group full → EVENT `CASINO_ENTER` per other member, REQUEST `LOCK_PLAYER_IN_GAME`
   ("Game starting…"), and after every RESPONSE 29 (or 3 s) EVENT `CASINO_MATCHED_UP`.
3. Each player moves to the room (`naf://rg.com:80/QM_n?hosted=yes|no`), is greeted once all
   are present, and sends channel 2 `00`, a channel-3 hello (`name`, `pid`, `roomname`), and
   REQUEST `SEAT_TAKE(pid)` on 0x15.
4. The bot answers RESPONSE `SEAT_TAKE` ok (this creates the client's game session,
   `0x100eb060` → `0x100ead20`) and sends EVENT `SEAT_TAKE` (string id) about every seated
   player to everyone; `SEAT_LEAVE` when a player leaves. Other channels are forwarded.

Both clients then wait for the first game-server message on channel 0x16, which nothing sends.

## The game protocol (channel 0x16) — reconstructed, not yet served

Channel 0x16 is **client ↔ authoritative game server**, not peer-to-peer lockstep.

* Framing: each MT message on 0x16 is one bare zlib stream (zlib 1.2.3, standard header,
  stream end required), no prefix. One message per MT packet. Receive `0x1005d100`
  (`conn->vt+0x7c(0x16)`), send `0x1005d1c0`.
* Message header (`0x1000c020` read, `0x1000d160` write), all `u32` LE: id, flag (1), kind,
  sender, N, recipient[N] (player keys, 0 = server), +0x30 (ignored), +0x34 turn, +0x38,
  +0x3c, +0x40, then the kind payload. Kinds 0–16 (2, 11, 13 invalid; table `0x1000c510`).
* Handshake: server M1 = kind 0 blob (`u32 len` + bytes) holding the whole world
  (`0x1003c4a0` dump, loader `0x1003e440`) + `u32` player key; client M2 = player info
  (name, hero, its player object); server M3 = slot; server M4 = battle state + queued events
  → the client is in the match. In-match messages go to the server, which re-addresses them
  and checks state hashes (mismatch → kind 10, `PLAYER_SYNC_ERROR`).
* Producer: only the engine's **server-mode controller** (`0x1000ecd0` ctor, vtable
  `0x101779a0`, per-frame `0x1000ddf0` → state machine `0x1000d8c0`). Sessions attach only
  through `0x1000d730`, called from the "TCP" acceptor `0x100df540`/`0x100df5f0` of the LAN
  host screen `0x100ca920` (listen URL `tcp://:12345`; join `tcp://%s:%u`). Single-player
  builds a controller too (`0x100debf0`, `0x100dea4a`, `0x100dee2d`), so the engine always
  plays against a local server. The DLL imports no socket API, so the `tcp://` transport
  has nothing underneath it in this build. Strings `MT_GAMESERVER`, `MT_HOSTMIGRATION`,
  `hosted`, `server`, `bluetooth` suggest the MT layer knew game-server and host roles.
* Rebuilding the server in Node is not realistic (world and battle formats are large), so
  the engine's own controller has to run somewhere.

## Next step: game server on the host player's device (plan "D")

**Goal:** the player the matchmaker marks `hosted=yes` also runs a **headless second engine
instance** in server mode (separate process, no rendering/input). It joins the relay room as
the game server; the `GameRoom` bot routes channel 0x16 between each player and it. Players'
clients stay unpatched (the server is the engine's own code), and the relay stays small enough
for the free Cloudflare Worker. Trade-offs accepted: the match ends if the host drops, and the
host has the latency advantage. The same server program can later run dedicated (VPS, fan PC).

### Phase 0 — prove the headless server (desktop, `C:\dev\rgbuild`)

Done = one `rgtest` client gets M1…M4 from a headless server instance and shows the match.

1. Read the LAN host path: `0x100ca920` (host screen) → `0x1000ecd0` (controller, called at
   `0x100ca99e`) and the acceptor `0x100df5f0`/`0x100df540` → `0x1000d730`. Record what the
   controller needs (world/level, hero choices, settings — compare the single-player callers
   `0x100dea4a`/`0x100dee2d` and `0x100debf0`/`0x100de940`) and what interface the session
   objects implement (they are the MT connections the clients' code sends on with
   `vt+0x68`/`vt+0x7c`).
2. Decide the server's transport (first open question). In order of preference:
   a. **The server engine is an MT peer in the relay room**, using the same SNAP/`net.c`
      path as clients: find whether the MT listen/accept can take a `naf://…/QM_n?…server…`
      URL (look at how `hosted`, `server`, `MT_GAMESERVER` are parsed near the URL parser
      and `0x10110210`/`0x1010fd80`), so each player's in-room connection becomes a session.
   b. Otherwise implement the `tcp://` transport the acceptor expects inside the recompiled
      runtime (host-side sockets under the MT transport vtable), and have a small bridge map
      relay players ↔ local TCP connections.
   c. Otherwise create session objects ourselves and call `0x1000d730` from the host.
3. Add a server mode to the desktop host (e.g. `native/host/server/rgserver.c`, or an
   `rgtest --server ROOM` flag): load the engine, skip menus, build the controller like the
   host screen does (`rt_guest_call`), run `tick` on the real clock without drawing. Watch for
   UI objects the controller assumes (the per-frame path may touch screens).
4. Debug with one `rgtest` client: `RECOMP_ARGS="--trace 1000d8c0,1000d730,1005d100,…"`,
   `RT_LIVE_TRIGGER`, relay `--log-packets`. Check the client's 40 s join window
   (`0x1000d730`: `timeGetTime()+40000`) and that M1 arrives only after the client's idle
   frame (`0x100ebed0`).

### Phase 1 — relay and room bot

1. Relay protocol: let a connection join a room as its **game server**
   (e.g. `{"t":"serve","room":"QM_n"}`), not counted as a player and not seated.
2. `GameRoom`: forward channel 0x16 (and whatever else the controller uses) between each
   player and the server member, tagged by player (binary prefix or a per-player virtual
   connection, depending on the Phase 0 transport); greet the server like a peer.
3. Casino: tell the hosted player's client to start the server (it already learns
   `hosted=yes` from `CASINO_MATCHED_UP`; the host-side launcher can key off that), and keep
   seated players waiting until the server member has joined (extend the 10 s start timeout).
4. Tests in `server/test/` for the server role, routing and server-leaves-room handling
   (all players get a clean disconnect).

### Phase 2 — launching the server on the host device

* **Windows:** build the headless server as an x64 recompiled `.exe` in `dist/`
  (`tools/build_windows.sh`); the Java client starts it (room name, relay URL, game dir) when
  it is matched with `hosted=yes`, and stops it after the match.
* **Android:** run the same code in a separate process (a service with `android:process`),
  since the runtime's guest memory and CPU state are global and a second engine cannot share
  the game's process. Add it to `tools/build_android.sh`.
* **Dedicated (optional):** `rgserver --relay ws://… --dedicated` registers with the relay and
  serves rooms when no host is available.

### Phase 3 — Android to Cloudflare

Add TLS (`wss://`) to `native/rt/net.c` so Android can use the Worker; until then Android
needs a self-hosted relay.

### Phase 4 — a full match

Two clients play a whole match: lockstep stays in sync (no `PLAYER_SYNC_ERROR`), the match
ends cleanly back in the casino, and host / non-host disconnects end or continue the match as
expected. Then 3–4 players.

### Open questions / risks

* Can the controller run without its UI (Phase 0.3)? Single-player suggests yes.
* Which transport (Phase 0.2): `naf://` room peer is the cheapest if the MT layer allows it.
* Does `hosted=yes` change the client's MT routing (e.g. expecting the game server on its
  own side)? Test a client with `hosted=no` for both players if routing gets in the way.
* Server CPU on low-end phones: the server does not draw, so it should be cheap.

Debugging tools: `tools/build_native_desktop.sh` (`EXTRA_CFLAGS=-DRT_CALLTRACE`,
`RECOMP_ARGS="--trace <hex,…>"`; delete `C:\dev\rgbuild\gen\rg_gen_table.c` to force
regeneration when only `RECOMP_ARGS` changed), `RT_LIVE_TRIGGER=<addr>:<n>`, `RT_NET_DEBUG=1`,
relay `--log-packets`. Two clients: `RG_NICK=Alice …` / `RG_NICK=Bob …` with keys
`250:ESC,350:DOWN,400:RETURN,700:RETURN,800:RETURN,900:RETURN` (`--realtime`).
