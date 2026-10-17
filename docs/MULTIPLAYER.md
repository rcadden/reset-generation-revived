# Multiplayer: our own server for Quick Match / Online

Status (2026-10-17):

| Piece | State |
|---|---|
| Relay server (`server/`, Node + Cloudflare Worker) | **Done**, 7 end-to-end tests |
| Client: Windows (Java replacement for `SNAPMobileClient`) | **Done** — logs in to the relay with a nickname, no Arena login screen |
| Client: native/Android (`native/rt/net.c`) | **Done** (plain `ws://` only) |
| Casino system (login, casino enter, Quick Match queue, matched-up) | **Done** — players reach the "Players in Match" screen of a shared game room |
| In-room protocol (seating, then the lockstep match itself) | **Open** — matched players see only themselves ("Waiting for Players…"); see "Next step" |
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
spoken. The relay sends each joiner one frame (`05 00 01 20 4e 00 00`); for rooms the
matchmaker created it waits until all matched players are present (max 10 s), because the
engine's handshake packets are lost on players who have not joined yet.

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
| REQUEST `CASINO_MATCHMAKING` | string hero (`PLUMBER`…), string random (`FALSE`) → RESPONSE generic + `u8 n` + n maps |
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

## Next step: the game room

After `CASINO_MATCHED_UP`, every player moves to the room, gets the greeting, and sends:
channel 2 `00`, a channel-3 hello (with its `pid` = profile `id`), channel 0x15
`REQUEST SEAT_TAKE(pid)`, then periodic pings. The host answers the channel-2 handshake
(`20`), but no client processes the seating messages (the engine's seating handler at
`0x100ce230` never runs), and the "Players in Match" list stays at one player.

Most likely, as with `CasinoSystem`, each SNAP game room had a **server-side game server**
that terminated the MT connections (handshake, seating, relaying between players), so
clients only ever talked to it, never directly to each other. The client DLL contains the
server-side request parsers, so the reconstruction path is the same as for the casino:

1. Trace the receive path for channels 2 and 0x15 (`RT_LIVE_TRIGGER=1010ff60:N` in an
   `RT_CALLTRACE` desktop build) to see why peer frames are dropped, and decode the
   channel-2 handshake states.
2. Implement a per-room game-server bot in `server/src/` (like `casino.js`) that answers the
   handshake and seating, announces seated players, and forwards game frames.
3. Verify with two `rgtest` clients (`RG_NICK=Alice …` / `RG_NICK=Bob …`, Quick Match
   keys) until the match starts, then check lockstep sync over a full match.

Tools for this: `tools/build_native_desktop.sh` with `EXTRA_CFLAGS=-DRT_CALLTRACE` and
`RECOMP_ARGS="--trace <hex,…>"` (function-entry logging, kv-key and connect-address dumps in
`native/rt/rt.c`), `RT_NET_DEBUG=1`, relay `--log-packets`.
