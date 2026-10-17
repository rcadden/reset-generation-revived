// The "CasinoSystem": the server-side matchmaker of the original N-Gage Arena, rebuilt from
// the engine's message classes (see docs/MULTIPLAYER.md, "Casino protocol").
//
// Wire format inside a relay packet (little-endian): one or more frames
//   [u16 length of the rest][u8 channel][payload]
// channel 3    key/value "command" (session hello on room join)
// channel 0x11 casino messages: [u8 type][u8 action] + fields
//              type 0 = REQUEST, 1 = RESPONSE, 2 = EVENT
// channel 1    keep-alive: u32 timeout in ms on join (20000), then 1-byte sequence pings
// channel 2    transport control: 1-byte replies to pings (same sequence), 00 on join
// Strings are the engine's string object: [u32 refcount|flags][u32 length][u32 capacity][bytes]
// (flags in bits 28..30: 2 = UTF-16).

export const CH_SYSTEM = 1;
export const CH_CONTROL = 2;
const KEEPALIVE_MS = 10_000;            // clients drop a peer that is silent for 20 s
const LOCK_TIMEOUT_MS = 3_000;          // wait this long for LOCK_PLAYER_IN_GAME answers
export const CH_COMMAND = 3;
export const CH_CASINO = 0x11;          // the casino system
export const CH_MATCHMAKER = 0x12;      // a matchmaker casino (kind 1)
export const CH_FRIENDLY = 0x13;        // a friendly casino (kind 2)
export const REQUEST = 0, RESPONSE = 1, EVENT = 2;

export const ACTIONS = {
  0: 'CASINOSYSTEM_LOGIN', 1: 'CASINOSYSTEM_LOGOUT', 2: 'CASINOSYSTEM_UPDATE', 3: 'CASINOSYSTEM_F_CASINO',
  4: 'GET_RANKINGS', 5: 'GET_STATISTICS', 6: 'GET_ACHIEVEMENTS', 7: 'GET_FRIENDLY_CASINO_LIST',
  8: 'CASINO_ENTER', 9: 'CASINO_MATCHMAKING', 10: 'CASINO_CANCEL_MATCHMAKING', 11: 'CASINO_EXIT',
  12: 'SEAT_RESERVE', 13: 'SEAT_TAKE', 14: 'SEAT_LEAVE', 15: 'SEAT_RESERVATION_EXPIRED',
  16: 'SEATING_FAILED', 17: 'GAME_START_FAILED', 18: 'TABLE_CLOSED', 19: 'TABLE_TERMINATED',
  20: 'CASINO_CREATE', 21: 'FRIENDLY_CASINO_ENTERED', 22: 'FRIENDLY_CASINO_LEFT', 23: 'FRIENDLY_CASINO_PLAYING',
  24: 'FRIENDLY_CASINO_UNAVAIL', 25: 'FRIENDLY_UPDATE', 26: 'CASINO_SHUTDOWN', 27: 'CASINO_ACTIVATE',
  28: 'FRIENDLY_CASINO_START', 29: 'LOCK_PLAYER_IN_GAME', 30: 'UNLOCK_PLAYER_FROM_GAME', 31: 'CASINO_MATCHED_UP',
  33: 'REGISTER_FRIENDS', 34: 'VERSION_NOT_SUPPORTED', 35: 'FRIEND_REMOVE', 36: 'FRIEND_ADD',
  37: 'CASINO_NOT_FOUND', 38: 'CASINO_CHECK_STATE', 39: 'PRESENCE_SUBSCRIBE', 40: 'PRESENCE_UNSUBSCRIBE',
  41: 'PRESENCE_ACCEPT', 42: 'PRESENCE_DECLINE',
};

/** Little-endian reader over a Uint8Array. */
export class Reader {
  constructor(bytes, pos = 0) { this.b = bytes; this.p = pos; this.v = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength); }
  get left() { return this.b.byteLength - this.p; }
  u8() { return this.v.getUint8(this.p++); }
  u16() { const x = this.v.getUint16(this.p, true); this.p += 2; return x; }
  u32() { const x = this.v.getUint32(this.p, true); this.p += 4; return x; }
  str() {
    const flags = this.u32(), len = this.u32();
    this.u32();
    const wide = ((flags >>> 28) & 7) === 2;
    const n = wide ? 2 * len : len;
    const raw = this.b.subarray(this.p, this.p + n);
    this.p += n;
    return wide ? String.fromCharCode(...new Uint16Array(raw.slice().buffer)) : String.fromCharCode(...raw);
  }
  kv() {
    const n = this.u32(), m = {};
    for (let i = 0; i < n; i++) { const k = this.str(); m[k] = this.str(); }
    return m;
  }
}

/** Little-endian writer. */
export class Writer {
  constructor() { this.parts = []; this.n = 0; }
  push(a) { this.parts.push(a); this.n += a.length; return this; }
  u8(x) { return this.push(Uint8Array.of(x & 255)); }
  u16(x) { return this.push(Uint8Array.of(x & 255, (x >>> 8) & 255)); }
  u32(x) { const a = new Uint8Array(4); new DataView(a.buffer).setUint32(0, x >>> 0, true); return this.push(a); }
  str(s) {
    const bytes = Uint8Array.from(String(s), (c) => c.charCodeAt(0) & 255);
    return this.u32(1).u32(bytes.length).u32(bytes.length).push(bytes);
  }
  kv(m) { const e = Object.entries(m); this.u32(e.length); for (const [k, v] of e) this.str(k).str(v); return this; }
  bytes() { const out = new Uint8Array(this.n); let o = 0; for (const p of this.parts) { out.set(p, o); o += p.length; } return out; }
}

/** A casino (lobby) descriptor: [u8 kind][u32 id][u32 a][u32 b][u32 c][string name].
 *  kind 1 = matchmaker (Quick Match), 2 = friendly casino. Field meanings a..c still unknown. */
export const MATCHMAKER = { kind: 1, id: 1, a: 0, b: 0, c: 0, name: 'QuickMatch' };
export function writeCasino(w, c) {
  return w.u8(c.kind).u32(c.id).u32(c.a).u32(c.b).u32(c.c).str(c.name);
}

/** Game settings object (kind 0): [u8 kind=0][u8 a][string s][u8 flag]. Meanings unknown. */
export function writeSettings(w, st = { a: 0, s: '', flag: 0 }) {
  return w.u8(0).u8(st.a).str(st.s).u8(st.flag);
}

/** A matched-up table: [string room][string address][u8 c][u8 d][u32 e][settings].
 *  address is an MT URL, e.g. naf://rg.com:80/QM_1?room=QM_1&hosted=yes ("hosted" decides
 *  whether this player creates the game room or joins it). */
export function writeTable(w, t) {
  return writeSettings(w.str(t.room).str(t.b).u8(t.c).u8(t.d).u32(t.e), t.settings);
}

/** Splits a relay packet into frames. */
export function parseFrames(bytes) {
  const frames = [];
  const r = new Reader(bytes);
  while (r.left >= 3) {
    const len = r.u16();
    if (len < 1 || len > r.left) break;
    const ch = r.u8();
    frames.push({ ch, data: bytes.subarray(r.p, r.p + len - 1) });
    r.p += len - 1;
  }
  return frames;
}

export function frame(ch, payload) {
  return new Writer().u16(payload.length + 1).u8(ch).push(payload).bytes();
}

export function hex(b) { return Array.from(b, (x) => x.toString(16).padStart(2, '0')).join(''); }

/**
 * Matchmaker bot for the CasinoSystem room. `send(client, bytes)` delivers a packet to one
 * player. Requests it does not understand are logged so the protocol can be extended.
 */
export class Casino {
  constructor({ send, log = () => {}, expectRoom = () => {} }) {
    this.send = send;
    this.expectRoom = expectRoom;
    this.log = log;
    this.players = new Map();     // client -> {name, state}
    this.queue = [];              // clients waiting in Quick Match (CASINO_MATCHMAKING)
    this.locking = new Map();     // client -> match waiting for LOCK_PLAYER_IN_GAME answers
    this.nextTable = 1;
    this.matchSize = 2;
    this.pingSeq = 1;
    this.timer = null;
  }

  /** Player profile key/value map the client reads (id, name, fameLevel, heroType, random). */
  profile(client) {
    const p = this.players.get(client) ?? {};
    return {
      id: String(p.id ?? 0), name: client.name, fameLevel: String(p.fameLevel ?? 0),
      heroType: p.hero ?? 'PLUMBER', random: p.random ?? 'FALSE',
    };
  }

  join(client) {
    this.nextPlayerId = (this.nextPlayerId ?? 0) + 1;
    this.players.set(client, { name: client.name, state: 'joined', id: this.nextPlayerId });
    // The client starts its casino session once the room's server has spoken. Like a peer,
    // the server opens with a transport hello (ch 2: 00) and its keep-alive timeout.
    this.send(client, frame(CH_CONTROL, Uint8Array.of(0)));
    this.send(client, frame(CH_SYSTEM, new Writer().u32(20000).bytes()));
    if (!this.timer && typeof setInterval === 'function') {
      this.timer = setInterval(() => this.keepAlive(), KEEPALIVE_MS);
      this.timer.unref?.();
    }
  }

  /** Pings every player so the engine does not time the casino out. */
  keepAlive() {
    if (!this.players.size) { clearInterval(this.timer); this.timer = null; return; }
    this.pingSeq = (this.pingSeq % 255) + 1;
    for (const c of this.players.keys()) this.send(c, frame(CH_SYSTEM, Uint8Array.of(this.pingSeq)));
  }

  leave(client) {
    if (this.queue.some((q) => q.client === client)) this.notifyQueue(client, 10);
    this.players.delete(client);
    this.queue = this.queue.filter((q) => q.client !== client);
  }

  /** EVENT CASINO_MATCHMAKING (9, [string id][profile]) / CASINO_CANCEL_MATCHMAKING (10, [string
   *  id]): updates the other waiting players' "Players in Match" list. */
  notifyQueue(client, action) {
    const p = this.profile(client);
    for (const q of this.queue) {
      if (q.client === client) continue;
      this.reply(q.client, EVENT, action, (w) => (action === 9 ? w.str(p.id).kv(p) : w.str(p.id)), q.ch);
    }
  }

  /**
   * Forms a match once enough players are queued. The "Players in Match" screen is up while a
   * player waits; it lists the others named by EVENT CASINO_MATCHMAKING. A full group is
   * announced in three steps:
   *   1. EVENT CASINO_ENTER [string id][profile] for each other member: the game room's seating
   *      events name players by id, resolved among the players the client's casino knows;
   *   2. REQUEST LOCK_PLAYER_IN_GAME (casino-system channel) -> "Game starting"; the client
   *      answers with a RESPONSE;
   *   3. once all have answered (or after a timeout), EVENT CASINO_MATCHED_UP with the room.
   */
  matchmake() {
    while (this.queue.length >= this.matchSize) {
      const group = this.queue.splice(0, this.matchSize);
      const room = `QM_${this.nextTable++}`;
      this.log(`casino: matched ${group.map((q) => q.client.name).join(', ')} -> ${room}`);
      for (const q of group) {
        for (const o of group) {
          if (o !== q) this.reply(q.client, EVENT, 8, (w) => w.str(this.profile(o.client).id).kv(this.profile(o.client)), q.ch);
        }
      }
      const match = { group, room, locked: new Set(), timer: null };
      for (const q of group) {
        this.locking.set(q.client, match);
        this.reply(q.client, REQUEST, 29, (w) => w.u32(0), CH_CASINO);
      }
      if (typeof setTimeout === 'function') {
        match.timer = setTimeout(() => this.matchedUp(match), LOCK_TIMEOUT_MS);
        match.timer.unref?.();
      }
    }
  }

  /** A player acknowledged LOCK_PLAYER_IN_GAME. */
  locked(client) {
    const match = this.locking.get(client);
    if (!match) return;
    match.locked.add(client);
    if (match.group.every((q) => match.locked.has(q.client))) this.matchedUp(match);
  }

  /** Tells each player of a locked group the game room to move to. */
  matchedUp(match) {
    if (match.done) return;
    match.done = true;
    clearTimeout(match.timer);
    const group = match.group.filter((q) => this.players.has(q.client));
    for (const q of match.group) this.locking.delete(q.client);
    const { room } = match;
    this.expectRoom(room, group.length, group.map((q) => this.profile(q.client)));
    group.forEach((q, i) => {
      const b = `naf://rg.com:80/${room}?hosted=${i === 0 ? 'yes' : 'no'}`;
      this.reply(q.client, EVENT, 31, (w) => writeTable(w.str(MATCHMAKER.name),
        { room, b, c: 0, d: 1, e: group.length, settings: { a: 0, s: b, flag: 0 } }), q.ch);
    });
  }

  packet(client, bytes) {
    for (const f of parseFrames(bytes)) {
      if (f.ch === CH_CASINO || f.ch === CH_MATCHMAKER || f.ch === CH_FRIENDLY) this.casinoMessage(client, f.data, f.ch);
      else if (f.ch === CH_COMMAND) this.log(`casino: ${client.name} command ${hex(f.data)}`);
      else if (f.ch === CH_SYSTEM && f.data.length === 1) this.send(client, frame(CH_CONTROL, f.data));   // pong
      else if (f.ch === CH_CONTROL && f.data.length === 1 && f.data[0] === 0) {
        this.send(client, frame(CH_CONTROL, Uint8Array.of(0x20)));     // accept the connection
      } else if (f.ch === CH_CONTROL || f.ch === CH_SYSTEM) { /* pong / keep-alive setting */ }
      else this.log(`casino: ${client.name} ch${f.ch} ${hex(f.data)}`);
    }
  }

  reply(client, type, action, body = (w) => w, ch = CH_CASINO) {
    const w = new Writer().u8(type).u8(action);
    body(w);
    this.send(client, frame(ch, w.bytes()));
  }

  /** Generic RESPONSE: [u8 result (0 = ok)][u32 value] */
  respond(client, action, result = 0, value = 0, extra = (w) => w, ch = CH_CASINO) {
    this.reply(client, RESPONSE, action, (w) => extra(w.u8(result).u32(value)), ch);
  }

  casinoMessage(client, data, ch) {
    const r = new Reader(data);
    const type = r.u8(), action = r.u8();
    const name = ACTIONS[action] ?? `ACTION_${action}`;
    this.log(`casino: ${client.name} ch${ch} ${['REQUEST', 'RESPONSE', 'EVENT'][type] ?? type} ${name} ${hex(data.subarray(2))}`);
    if (type === RESPONSE && action === 29) return this.locked(client);
    if (type !== REQUEST) return;
    switch (action) {
      case 0: {   // CASINOSYSTEM_LOGIN: u32 version, string client version
        const version = r.u32(), clientVersion = r.str();
        this.log(`casino: login ${client.name} protocol ${version} (${clientVersion})`);
        // response: generic + profile key/value map + u8 has-casino + casino descriptor
        this.respond(client, 0, 0, 0, (w) => writeCasino(w.kv(this.profile(client)).u8(1), MATCHMAKER));
        break;
      }
      case 5:     // GET_STATISTICS -> generic + stats: string, 9 x u32, u32 count (+ items)
        this.respond(client, 5, 0, 0, (w) => { w.str(client.name); for (let i = 0; i < 9; i++) w.u32(0); return w.u32(0); }, ch);
        break;
      case 6:     // GET_ACHIEVEMENTS -> generic + 200-byte achievement block (none unlocked online)
        this.respond(client, 6, 0, 0, (w) => w.push(new Uint8Array(200)), ch);
        break;
      case 8: {   // CASINO_ENTER: u32 casino id -> generic + key/value map + casino descriptor
        const id = r.u32();
        this.respond(client, 8, 0, id, (w) => writeCasino(w.kv(this.profile(client)), MATCHMAKER), ch);
        break;
      }
      case 9: {   // CASINO_MATCHMAKING: string hero, string flag -> generic + u8 count + kv maps
        const hero = r.str(), flag = r.str();
        const p = this.players.get(client);
        if (p) { p.hero = hero; p.random = flag; }
        // response: generic + u8 n + profiles of the players already waiting
        this.queue = this.queue.filter((q) => q.client !== client);
        const waiting = this.queue.map((q) => this.profile(q.client));
        this.respond(client, 9, 0, 0, (w) => { w.u8(waiting.length); for (const m of waiting) w.kv(m); return w; }, ch);
        this.queue.push({ client, hero, flag, ch });
        this.notifyQueue(client, 9);
        this.matchmake();
        break;
      }
      case 10:    // CASINO_CANCEL_MATCHMAKING
        this.notifyQueue(client, 10);
        this.queue = this.queue.filter((q) => q.client !== client);
        this.respond(client, 10, 0, 0, (w) => w, ch);
        break;
      default:
        this.respond(client, action, 0, 0, (w) => w, ch);
    }
  }
}
