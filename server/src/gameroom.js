// Game-room server: the per-room "game server" of the original N-Gage Arena, rebuilt from the
// engine's table client (see docs/MULTIPLAYER.md, "The game room").
//
// Every matchmade room gets one. Like the casino bot it is an MT peer to each player: it opens
// with the transport hello and keep-alive, answers pings, and serves the seating protocol on
// channel 0x15 (same message framing as the casino: [u8 type][u8 action] + fields):
//   REQUEST  SEAT_RESERVE / SEAT_TAKE / SEAT_LEAVE  u32 player id
//   RESPONSE generic: [u8 result (0 = ok)][u32 value]
//   EVENT    SEAT_RESERVE / SEAT_TAKE / SEAT_LEAVE  string player id (decimal); the client looks
//            the id up among the players its casino knows (EVENT CASINO_ENTER), so the casino
//            bot introduces matched players to each other before CASINO_MATCHED_UP.
// Frames on other channels are the match itself and are forwarded to the other players.

import { CH_SYSTEM, CH_CONTROL, CH_COMMAND, RESPONSE, REQUEST, EVENT, ACTIONS, Reader, Writer, parseFrames, frame, hex } from './casino.js';

export const CH_TABLE = 0x15;
export const SEAT_RESERVE = 12, SEAT_TAKE = 13, SEAT_LEAVE = 14;
const KEEPALIVE_MS = 10_000;

export class GameRoom {
  /**
   * send(client, bytes) delivers a packet to one player; `expected` is the number of matched
   * players: the room stays silent until they are all present (or `start()` is called), because
   * the engine's handshake is lost on players who have not joined yet.
   */
  constructor({ name, send, log = () => {}, expected = 0, profiles = [] }) {
    this.name = name;
    this.send = send;
    this.log = log;
    this.expected = expected;
    this.profiles = profiles;   // matched players' casino profiles
    this.players = new Map();   // client -> {greeted, pid, seat}
    this.started = false;
    this.pingSeq = 1;
    this.timer = null;
  }

  join(client) {
    this.players.set(client, { greeted: false, pid: null, seat: null });
    if (this.started || this.players.size >= this.expected) this.start();
  }

  /** Greets everyone not yet greeted: transport hello (ch 2: 00) and keep-alive timeout. */
  start() {
    this.started = true;
    for (const [c, p] of this.players) {
      if (p.greeted) continue;
      p.greeted = true;
      this.send(c, new Uint8Array([...frame(CH_CONTROL, Uint8Array.of(0)), ...frame(CH_SYSTEM, new Writer().u32(20000).bytes())]));
    }
    if (!this.timer && typeof setInterval === 'function') {
      this.timer = setInterval(() => this.keepAlive(), KEEPALIVE_MS);
      this.timer.unref?.();
    }
  }

  keepAlive() {
    this.pingSeq = (this.pingSeq % 255) + 1;
    for (const [c, p] of this.players) if (p.greeted) this.send(c, frame(CH_SYSTEM, Uint8Array.of(this.pingSeq)));
  }

  leave(client) {
    const p = this.players.get(client);
    this.players.delete(client);
    if (p?.seat) this.event(SEAT_LEAVE, p.pid);
    if (!this.players.size) this.close();
  }

  close() {
    if (this.timer) clearInterval(this.timer);
    this.timer = null;
  }

  packet(client, bytes) {
    const p = this.players.get(client);
    if (!p) return;
    const forward = [];
    for (const f of parseFrames(bytes)) {
      if (f.ch === CH_TABLE) this.tableMessage(client, p, f.data);
      else if (f.ch === CH_SYSTEM && f.data.length === 1) this.send(client, frame(CH_CONTROL, f.data));   // pong
      else if (f.ch === CH_CONTROL && f.data.length === 1 && f.data[0] === 0) this.send(client, frame(CH_CONTROL, Uint8Array.of(0x20)));
      else if (f.ch === CH_CONTROL || f.ch === CH_SYSTEM) { /* pong / keep-alive setting */ }
      else if (f.ch === CH_COMMAND) this.command(client, p, f.data);
      else forward.push(frame(f.ch, f.data));
    }
    if (forward.length) {
      const out = new Uint8Array(forward.reduce((n, f) => n + f.length, 0));
      let o = 0;
      for (const f of forward) { out.set(f, o); o += f.length; }
      this.log(`room ${this.name}: ${client.name} -> others ${hex(out).slice(0, 80)}`);
      for (const c of this.players.keys()) if (c !== client) this.send(c, out);
    }
  }

  /** Channel-3 session hello: key/value pairs (name, pid, passwd, roomname, ...). */
  command(client, p, data) {
    try {
      // [u8 n] n x (string key, string value), then typed extras we do not need
      const r = new Reader(data);
      const kv = {};
      for (let n = r.u8(); n > 0; n--) { const k = r.str(); kv[k] = r.str(); }
      if (kv.pid) p.pid ??= kv.pid;
      this.log(`room ${this.name}: ${client.name} hello ${JSON.stringify(kv)}`);
    } catch {
      this.log(`room ${this.name}: ${client.name} command ${hex(data)}`);
    }
  }

  tableMessage(client, p, data) {
    const r = new Reader(data);
    const type = r.u8(), action = r.u8();
    const name = ACTIONS[action] ?? `ACTION_${action}`;
    this.log(`room ${this.name}: ${client.name} ${['REQUEST', 'RESPONSE', 'EVENT'][type] ?? type} ${name} ${hex(data.subarray(2))}`);
    if (type !== REQUEST) return;
    const pid = r.left >= 4 ? String(r.u32()) : p.pid;
    p.pid = pid;
    switch (action) {
      case SEAT_RESERVE:
      case SEAT_TAKE: {
        const first = !p.seat;
        p.seat = action === SEAT_TAKE ? 'taken' : (p.seat ?? 'reserved');
        this.respond(client, action, 0, Number(pid));
        // the newcomer learns about everyone already seated, then everyone learns about it
        if (first) for (const [c, o] of this.players) if (c !== client && o.seat) this.sendEvent(client, o.seat === 'taken' ? SEAT_TAKE : SEAT_RESERVE, o.pid);
        this.event(action, pid);
        break;
      }
      case SEAT_LEAVE:
        p.seat = null;
        this.respond(client, action, 0, Number(pid));
        this.event(SEAT_LEAVE, pid);
        break;
      default:
        this.respond(client, action, 0, 0);
    }
  }

  respond(client, action, result = 0, value = 0) {
    this.send(client, frame(CH_TABLE, new Writer().u8(RESPONSE).u8(action).u8(result).u32(value).bytes()));
  }

  sendEvent(client, action, pid) {
    this.send(client, frame(CH_TABLE, new Writer().u8(EVENT).u8(action).str(String(pid)).bytes()));
  }

  /** Tells every greeted player about a seat change. */
  event(action, pid) {
    for (const [c, p] of this.players) if (p.greeted) this.sendEvent(c, action, pid);
  }
}
