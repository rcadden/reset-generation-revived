// Reset Generation Revived multiplayer relay: named rooms + ordered broadcast.
//
// Transport-independent core shared by the Node server (node/server.js) and the
// Cloudflare Worker (worker/index.js). The game engine runs its own matchmaking
// and lockstep game logic on top of SNAP Mobile-style "game rooms"; the server only
// needs rooms (max 4 players), membership, ordered broadcast and disconnect notices.
//
// Protocol (WebSocket; see docs/MULTIPLAYER.md):
//   text frames  = JSON control messages {"t": <type>, ...}
//   binary frames = one game packet (<= MAX_PACKET bytes), relayed to the other
//                   members of the sender's room in arrival order.

export const PROTOCOL_VERSION = 1;
export const MAX_PACKET = 2048;          // the engine's shared net buffer
export const MAX_ROOM_MEMBERS = 4;       // createGameRoom(name, null, 4, ...)
export const MAX_NAME = 15;              // SMArenaValidator
// Permanent lobby rooms the original SNAP server provided. The engine joins "CasinoSystem"
// first and runs its matchmaking there (broadcast packets), then moves to a 4-player room.
export const LOBBY_ROOMS = { CasinoSystem: 1000 };
const NAME_RE = /^[A-Za-z0-9_]{1,15}$/;
const ROOM_RE = /^[\x20-\x7e]{1,64}$/;

/** One connected client. `send(data)` takes a string (control) or Uint8Array (packet). */
class Client {
  constructor(id, send, close) {
    this.id = id;
    this.send = send;
    this.close = close;
    this.name = null;
    this.room = null;
    this.lastSeen = Date.now();
  }
  ctl(msg) {
    this.send(JSON.stringify(msg));
  }
}

export class Relay {
  constructor({ log = () => {}, now = () => Date.now() } = {}) {
    this.clients = new Map();   // id -> Client
    this.rooms = new Map();     // lower-case room name -> {name, members: Set<Client>, created}
    this.nextId = 1;
    this.log = log;
    this.now = now;
    this.stats = { packets: 0, bytes: 0, connections: 0 };
    for (const [name, max] of Object.entries(LOBBY_ROOMS)) {
      this.rooms.set(name.toLowerCase(), { key: name.toLowerCase(), name, members: new Set(), created: this.now(), max, permanent: true });
    }
  }

  /** Register a new connection; returns the handlers the transport must call. */
  connect(send, close) {
    const c = new Client(`c${this.nextId++}`, send, close);
    this.clients.set(c.id, c);
    this.stats.connections++;
    return {
      client: c,
      onText: (text) => this.onText(c, text),
      onBinary: (bytes) => this.onBinary(c, bytes),
      onClose: () => this.disconnect(c),
    };
  }

  onText(c, text) {
    c.lastSeen = this.now();
    let m;
    try {
      m = JSON.parse(text);
    } catch {
      return c.ctl({ t: 'error', code: 'bad_json' });
    }
    if (!m || typeof m.t !== 'string') return c.ctl({ t: 'error', code: 'bad_message' });
    if (m.t !== 'hello' && m.t !== 'ping' && !c.name) {
      return c.ctl({ t: 'error', op: m.t, code: 'not_logged_in' });
    }
    switch (m.t) {
      case 'hello': return this.hello(c, m);
      case 'ping': return c.ctl({ t: 'pong', ts: m.ts ?? null });
      case 'create': return this.enter(c, m.room, 'create');
      case 'join': return this.enter(c, m.room, 'join');
      case 'move': return this.enter(c, m.room, 'move');
      case 'leave': return this.leave(c, 'left');
      case 'rooms': return c.ctl({ t: 'rooms', rooms: this.roomList() });
      default: return c.ctl({ t: 'error', op: m.t, code: 'unknown_type' });
    }
  }

  hello(c, m) {
    if (m.v !== PROTOCOL_VERSION) {
      c.ctl({ t: 'error', op: 'hello', code: 'version', server: PROTOCOL_VERSION });
      return c.close(4000, 'protocol version');
    }
    if (typeof m.name !== 'string' || !NAME_RE.test(m.name)) {
      return c.ctl({ t: 'error', op: 'hello', code: 'bad_name' });
    }
    // Names are display names only (no accounts yet); make them unique per server.
    let name = m.name;
    const taken = new Set([...this.clients.values()].filter((o) => o !== c && o.name).map((o) => o.name.toLowerCase()));
    for (let i = 2; taken.has(name.toLowerCase()); i++) {
      const suffix = String(i);
      name = m.name.slice(0, MAX_NAME - suffix.length) + suffix;
    }
    c.name = name;
    this.log(`hello ${c.id} as ${name}`);
    c.ctl({ t: 'welcome', v: PROTOCOL_VERSION, id: c.id, name });
  }

  /**
   * create: make the room (joins it if it already exists and has space; two players
   *         creating the same private room name should meet, not fail).
   * join:   only an existing room; error 'not_found' / 'full' otherwise.
   * move:   join if it exists, else create (SNAPMobileClient.moveToRoom).
   */
  enter(c, roomName, op) {
    if (typeof roomName !== 'string' || !ROOM_RE.test(roomName)) {
      return c.ctl({ t: 'error', op, code: 'bad_room' });
    }
    const key = roomName.toLowerCase();
    if (c.room) {
      if (c.room.key === key) return c.ctl({ t: 'joined', op, room: c.room.name, members: this.memberNames(c.room) });
      this.leave(c, 'left');
    }
    let room = this.rooms.get(key);
    if (!room) {
      if (op === 'join') return c.ctl({ t: 'error', op, room: roomName, code: 'not_found' });
      room = { key, name: roomName, members: new Set(), created: this.now(), max: MAX_ROOM_MEMBERS, permanent: false };
      this.rooms.set(key, room);
      this.log(`room + ${roomName}`);
    } else if (room.members.size >= room.max) {
      return c.ctl({ t: 'error', op, room: roomName, code: 'full' });
    }
    room.members.add(c);
    c.room = room;
    this.log(`${c.name} -> ${room.name} (${room.members.size})`);
    c.ctl({ t: 'joined', op, room: room.name, members: this.memberNames(room) });
    for (const o of room.members) if (o !== c) o.ctl({ t: 'peer_joined', room: room.name, name: c.name });
  }

  leave(c, reason) {
    const room = c.room;
    if (!room) return reason === 'left' ? c.ctl({ t: 'left', room: null }) : undefined;
    room.members.delete(c);
    c.room = null;
    if (reason === 'left') c.ctl({ t: 'left', room: room.name });
    for (const o of room.members) o.ctl({ t: 'peer_left', room: room.name, name: c.name, reason });
    if (room.members.size === 0 && !room.permanent) {
      this.rooms.delete(room.key);
      this.log(`room - ${room.name}`);
    }
  }

  onBinary(c, bytes) {
    c.lastSeen = this.now();
    if (!c.room) return c.ctl({ t: 'error', op: 'data', code: 'not_in_room' });
    if (bytes.byteLength === 0 || bytes.byteLength > MAX_PACKET) {
      return c.ctl({ t: 'error', op: 'data', code: 'bad_size' });
    }
    this.stats.packets++;
    this.stats.bytes += bytes.byteLength;
    for (const o of c.room.members) if (o !== c) o.send(bytes);
  }

  disconnect(c) {
    if (!this.clients.has(c.id)) return;
    this.leave(c, 'disconnected');
    this.clients.delete(c.id);
    this.log(`bye ${c.id} ${c.name ?? ''}`);
  }

  /** Close connections idle for longer than `ms` (no frames, including pings). */
  reapIdle(ms) {
    const cutoff = this.now() - ms;
    for (const c of [...this.clients.values()]) {
      if (c.lastSeen < cutoff) {
        this.disconnect(c);
        try { c.close(4001, 'idle'); } catch { /* already gone */ }
      }
    }
  }

  memberNames(room) {
    return [...room.members].map((m) => m.name);
  }

  roomList() {
    return [...this.rooms.values()].map((r) => ({ name: r.name, players: r.members.size, max: r.max, lobby: r.permanent }));
  }

  status() {
    return { players: this.clients.size, rooms: [...this.rooms.values()].filter((r) => !r.permanent).length, ...this.stats };
  }
}
