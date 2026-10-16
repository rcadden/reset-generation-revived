# Multiplayer: can we run our own server?

**Short answer: yes, and it is a small server.** The game's netcode is peer-to-peer *through a
relay*: the engine does its own matchmaking and game logic on top of generic "named rooms" and
broadcast "game packets". The server never runs game rules. Its simulation is deterministic
fixed-point integer code, so clients stay in sync by exchanging inputs.

## How online worked in 2008–2010

* Service: Nokia **SNAP Mobile** (client library v1.4.4 in `lib/rg-signed.jar`), branded
  N-Gage Arena. Gateway `resetgeneration-gw.arena.n-gage.com`, lobby `resgen`, game class
  `42713`, connectors `tcp,http:80`. Shut down with N-Gage Arena (2010).
* Layers:

```
native engine  ──(JNI: NativeSNAPMobileInterface / NativeSNAPMobileEventListener)──►
  Java SNAPMobileClient (RedLynx glue: action queue + event queue, own thread)
    ──► com.nokia.sm.net.ServerComm (SNAP protocol) ──► TCP :8976 or HTTP POST /perl/sm
```

### The engine-facing contract (what we must provide)

Engine → host calls (`NativeSNAPMobileInterface`):

| Call | Meaning |
|---|---|
| `startLoginScreen()`, `stopLoginScreen()`, `getLoginScreenStatus()`, `login()`, `logout()`, `isLoggedIn()`, `loginWasCancelled()` | Account login flow (status codes 0–7: in login screen … logged in, failed, auth error, could not connect) |
| `createRoom(name)`, `joinRoom(name)`, `moveToRoom(name)`, `leaveRoom(name)` | Named game rooms, max 4 players (`createGameRoom(name, null, 4, 1, -1)`) |
| `send(len)` | Broadcast `len` bytes from the shared 2 KB `netBuffer` to the room |
| `getFriendList()`, `send/accept/decline/removeFriend(name)`, `setPresence(availability, text)` | Friends and presence (optional for a first server) |

Host → engine callbacks (`NativeSNAPMobileEventListener`): `onArenaLoginCompleted/Cancelled`,
`onArenaLogoutCompleted`, `onConnected` (joined room), `onDataReceived(len, roomName)` (bytes
placed in the shared buffer), `onDisconnectedFromGameRoom(room)`, `onDisconnectedFromNAF(code)`,
`onFriendList(names[], presences[], states[])`, `onFriendRequest*`, `onFriendRemoved`,
`onPresence(name, availability, text)`, `onError(code, text)`.

The engine's own room-naming scheme handles quick match / private games, so the server only
needs rooms, membership, ordered broadcast and disconnect notification.

### The original wire protocol (for reference / a compatible server)

* **Requests**: URL-encoded text `V=<ver>&S=<snapSessionID>&C=<command>&P1=…&P2=…` — over HTTP
  (`POST http://<gw>:80/perl/sm?`) or a framed TCP connection to port **8976** (hello/hello_ack
  handshake, 16-bit request IDs, async callbacks). Some parameters (passwords, keys) are
  obfuscated with a simple per-command rotate-and-XOR using 16 hard-coded keys (implemented in
  `decompiled/rg-signed/i.java`, generated locally), others are base64.
* **Responses**: first line `200 OK` (or an error code + message), then fields separated by
  `|`, `,` and newlines in a fixed order per command; the field schemas for every command are
  tabled in `decompiled/rg-signed/r.java` (e.g. login returns
  `webSessionID|impsSessionID|snapSessionID|snapUserID|buddyList|messageList`).
* **Events** are delivered by polling `retrieveAllEvents` (command 3) / listening; each event
  line starts with an event ID: 103/104 user joined/left lobby, 105/106 room created/deleted,
  107/108 user joined/left room, 109–111 attribute/property changes, 112–115 chat,
  **116/117 game data** (`from|+gameData`, base64), 301–312 IM/presence/buddy events.
* Commands used by Reset Generation: 1 `extendedLogin`, 104 `logoutClient`, 109 `joinLobby`,
  113 `selectGameRooms`, 116 `createGameRoom`, 117 `joinGameRoom`, 118 `leaveGameRoom`,
  125 `getGameRooms`, 127 `getUsersInGameRoom`, 133 `sendGamePacketToGameServer`,
  3/130/311 event retrieval, plus buddy/presence commands.

## Options for a new server

| Option | Server | Client changes | Verdict |
|---|---|---|---|
| 1. **SNAP-compatible emulator** | Re-implement the ~15 SNAP commands above, text protocol, TCP 8976 + HTTP | None in the game code: launch with `-gw <our-host>` (the wrapper already supports it) | Works with the stock Java client, but locks us into the 2008 protocol and its quirks; awkward on Android where the host is native. |
| 2. **Own protocol (recommended)** | Small WebSocket room relay: `hello{name}`, `join{room}`, `leave`, `data{bytes}` → broadcast to other members in order, `members`/`left` notifications, heartbeat | Replace the Java `SNAPMobileClient` with a thin client that implements the same engine-facing contract (table above) over WebSocket; later the native host (Android/Windows) implements it in C | Simple, modern, TLS, works through firewalls/mobile networks, one implementation for Windows and Android, cross-play. |

### Recommended architecture

```
Game (Windows Java build, later native host, Android)
   └─ RevivedNet client: implements NativeSNAPMobileInterface contract
         └─ WebSocket (wss://rg.<domain>/ws)
               └─ Cloudflare Worker (router) → Durable Object per room name
                     · ordered broadcast of data frames to the other ≤3 members
                     · join/leave/disconnect events, idle-room cleanup
                     · guest identity (nickname + random ID); optional account later
```

Cloudflare Workers + Durable Objects fit the workspace's stack (Cloudflare preferred), cost
nothing at hobby scale, and a Durable Object per room gives exactly the ordered, single-writer
semantics the relay needs. A Node.js server (~200 lines) is an equivalent self-host option.

Login: replace the Arena username/password screen with a nickname prompt (or skip it entirely
and use a stored random nickname). Friends/presence can come later (Supabase table
`resetgen__friends`, per the one-Supabase-project rule).

### Work breakdown

1. **Capture the packet format** — run two local instances with a logging relay to confirm packet
   sizes/rates and that games stay in sync (determinism check). The engine's `send()` limit is
   2 KB per packet.
2. **Relay server** (Durable Object or Node) with room lifecycle and tests.
3. **Java client** replacing `SNAPMobileClient` (keep the action/event queue structure so the
   engine sees identical callback timing); nickname login UI; hide friends UI until supported.
4. **Native host client** (C, for Android/Windows native host) implementing the same contract.
5. Nice-to-haves: reconnect within a match, simple server-side room listing for a lobby browser,
   ranking/stats (the original had `reportScores`/rankings).

Estimated effort: 1–2 weeks for options 2.1–2.3 (desktop cross-play), +1 week for the native client.

## Open questions

* Whether matches use a host/authority player or pure lockstep (affects disconnect handling).
  To be confirmed in step 1 by logging packets.
* Version compatibility: the N-Gage builds cannot connect (they talk to Nokia's dead servers
  with DRM'd clients) — cross-play is between our builds only.
