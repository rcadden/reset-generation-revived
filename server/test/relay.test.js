// End-to-end tests over real WebSockets. Starts node/server.js on a free port, or uses
// RELAY_URL (e.g. ws://127.0.0.1:8787/ws from `npm run dev:worker`) to test the Worker.
import { test, before, after } from 'node:test';
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import WebSocket from 'ws';

let url = process.env.RELAY_URL;
let proc;

before(async () => {
  if (url) return;
  const port = 20000 + Math.floor(Math.random() * 20000);
  const script = fileURLToPath(new URL('../node/server.js', import.meta.url));
  proc = spawn(process.execPath, [script, '--port', String(port), '--host', '127.0.0.1', '--quiet'], { stdio: ['ignore', 'pipe', 'inherit'] });
  await new Promise((resolve) => proc.stdout.on('data', (d) => { if (String(d).includes('listening')) resolve(); }));
  url = `ws://127.0.0.1:${port}/ws`;
});

after(() => proc?.kill());

const GREETING = Buffer.from('050001204e0000', 'hex');

/** A test client that queues every message it receives. */
async function client(name, keepGreeting = false) {
  const ws = new WebSocket(url);
  const queue = [];
  const waiters = [];
  ws.on('message', (data, isBinary) => {
    if (isBinary && !keepGreeting && Buffer.from(data).equals(GREETING)) return;   // room server hello
    const msg = isBinary ? { binary: new Uint8Array(data) } : JSON.parse(String(data));
    const w = waiters.shift();
    if (w) w(msg); else queue.push(msg);
  });
  await new Promise((r, j) => { ws.on('open', r); ws.on('error', j); });
  const c = {
    ws,
    send: (m) => ws.send(JSON.stringify(m)),
    next: (ms = 2000) => queue.length ? Promise.resolve(queue.shift()) : new Promise((r, j) => {
      const t = setTimeout(() => j(new Error(`${name}: no message`)), ms);
      waiters.push((m) => { clearTimeout(t); r(m); });
    }),
    quiet: async (ms = 150) => { await new Promise((r) => setTimeout(r, ms)); assert.deepEqual(queue, [], `${name} got unexpected messages`); },
    noBinary: async (ms = 150) => { await new Promise((r) => setTimeout(r, ms)); assert.ok(!queue.some((m) => m.binary), `${name} got an unexpected packet`); },
  };
  if (name) {
    c.send({ t: 'hello', v: 1, name });
    const w = await c.next();
    assert.equal(w.t, 'welcome');
    c.name = w.name;
  }
  return c;
}

test('hello validates version and name', async () => {
  const c = await client(null);
  c.send({ t: 'join', room: 'x' });
  assert.equal((await c.next()).code, 'not_logged_in');
  c.send({ t: 'hello', v: 1, name: 'bad name!' });
  assert.equal((await c.next()).code, 'bad_name');
  c.send({ t: 'hello', v: 99, name: 'ok' });
  assert.equal((await c.next()).code, 'version');
});

test('duplicate names get a numeric suffix', async () => {
  const a = await client('Twin');
  const b = await client('Twin');
  assert.equal(a.name, 'Twin');
  assert.equal(b.name, 'Twin2');
  a.ws.close(); b.ws.close();
});

test('join, broadcast to others in order, leave', async () => {
  const a = await client('Alice');
  const b = await client('Bob');
  const c = await client('Carol');

  b.send({ t: 'join', room: 'Room1' });
  assert.equal((await b.next()).code, 'not_found');

  a.send({ t: 'create', room: 'Room1' });
  assert.deepEqual(await a.next(), { t: 'joined', op: 'create', room: 'Room1', members: ['Alice'] });
  b.send({ t: 'join', room: 'room1' });          // names are case-insensitive
  assert.deepEqual((await b.next()).members, ['Alice', 'Bob']);
  assert.deepEqual(await a.next(), { t: 'peer_joined', room: 'Room1', name: 'Bob' });
  c.send({ t: 'move', room: 'Other' });           // move creates a missing room
  assert.equal((await c.next()).room, 'Other');

  for (let i = 0; i < 50; i++) a.ws.send(Uint8Array.of(i, i * 2, 7));
  for (let i = 0; i < 50; i++) assert.deepEqual((await b.next()).binary, Uint8Array.of(i, i * 2, 7));
  await a.quiet();                                 // no echo to the sender
  await c.quiet();                                 // other rooms see nothing

  b.send({ t: 'leave' });
  assert.equal((await b.next()).t, 'left');
  assert.deepEqual(await a.next(), { t: 'peer_left', room: 'Room1', name: 'Bob', reason: 'left' });
  a.ws.close(); b.ws.close(); c.ws.close();
});

test('rooms hold at most four players; disconnect frees a slot', async () => {
  const ps = [];
  for (let i = 0; i < 5; i++) ps.push(await client(`P${i}`));
  ps[0].send({ t: 'create', room: 'Full' });
  await ps[0].next();
  for (let i = 1; i < 4; i++) {
    ps[i].send({ t: 'join', room: 'Full' });
    assert.equal((await ps[i].next()).t, 'joined');
  }
  ps[4].send({ t: 'join', room: 'Full' });
  assert.equal((await ps[4].next()).code, 'full');
  ps[3].ws.close();
  for (let i = 0; i < 3; i++) {
    let m;
    do { m = await ps[i].next(); } while (m.t === 'peer_joined');
    assert.deepEqual(m, { t: 'peer_left', room: 'Full', name: 'P3', reason: 'disconnected' });
  }
  ps[4].send({ t: 'join', room: 'Full' });
  assert.equal((await ps[4].next()).members.length, 4);
  for (const p of ps) p.ws.close();
});

test('the CasinoSystem lobby always exists and is not limited to four', async () => {
  const ps = [];
  for (let i = 0; i < 6; i++) {
    ps.push(await client(`L${i}`));
    ps[i].send({ t: 'join', room: 'CasinoSystem' });
    assert.equal((await ps[i].next()).t, 'joined');
  }
  for (const p of ps) p.ws.close();
  await new Promise((r) => setTimeout(r, 100));
  const c = await client('Late');
  c.send({ t: 'join', room: 'CasinoSystem' });
  assert.deepEqual((await c.next()).members, ['Late']);
  c.ws.close();
});

test('room list, ping and packet limits', async () => {
  const a = await client('Lister');
  a.ws.send(Uint8Array.of(1));
  assert.equal((await a.next()).code, 'not_in_room');
  a.send({ t: 'create', room: 'Listed' });
  await a.next();
  a.send({ t: 'rooms' });
  const r = await a.next();
  assert.ok(r.rooms.some((x) => x.name === 'Listed' && x.players === 1 && x.max === 4 && !x.lobby));
  assert.ok(r.rooms.some((x) => x.name === 'CasinoSystem' && x.lobby));
  a.ws.send(new Uint8Array(2049));
  assert.equal((await a.next()).code, 'bad_size');
  a.send({ t: 'ping', ts: 42 });
  assert.deepEqual(await a.next(), { t: 'pong', ts: 42 });
  a.ws.close();
});

// ---------------------------------------------------------------- casino (matchmaker bot)
import { Writer, Reader, parseFrames, frame } from '../src/casino.js';

const ROOM_GREETING = Buffer.from('02000200050001204e0000', 'hex');   // room: transport hello + keep-alive
const CH_TABLE = 0x15, REQUEST = 0, RESPONSE = 1, EVENT = 2;           // game room seating channel
const SEAT_TAKE = 13, SEAT_LEAVE = 14;

function casinoRequest(ch, action, body = (w) => w) {
  return frame(ch, body(new Writer().u8(0).u8(action)).bytes());
}

/** A seating message on the table channel: [u8 type][u8 action] + fields. */
function tableMsg(type, action, fill = (w) => w) {
  return Buffer.from(frame(CH_TABLE, fill(new Writer().u8(type).u8(action)).bytes()));
}

async function nextCasino(c, ms = 2000) {
  for (;;) {
    const m = await c.next(ms);
    if (!m.binary) continue;
    for (const f of parseFrames(m.binary)) {
      if (f.ch >= 0x11) { const r = new Reader(f.data); return { ch: f.ch, type: r.u8(), action: r.u8(), r }; }
    }
  }
}

/** Next binary packet; JSON control messages are skipped. */
async function nextBinary(c) {
  for (;;) {
    const m = await c.next();
    if (m.binary) return Buffer.from(m.binary);
  }
}

/** Connects, joins the CasinoSystem lobby and logs in and enters the casino. */
async function signIn(name) {
  const p = await client(name, true);
  p.send({ t: 'join', room: 'CasinoSystem' });
  assert.equal((await p.next()).t, 'joined');
  assert.deepEqual(Buffer.from((await p.next()).binary), Buffer.from('02000200', 'hex'));  // transport hello
  assert.deepEqual(Buffer.from((await p.next()).binary), GREETING);
  p.ws.send(casinoRequest(0x11, 0, (w) => w.u32(1).str('Reset Generation v. 1.1.3')));
  const login = await nextCasino(p);
  assert.deepEqual([login.ch, login.type, login.action, login.r.u8()], [0x11, 1, 0, 0]);
  login.r.u32();
  const profile = login.r.kv();
  assert.equal(profile.name, name);
  assert.ok('fameLevel' in profile && 'heroType' in profile);
  assert.equal(login.r.u8(), 1);                                 // has casino
  assert.equal(login.r.u8(), 1);                                 // kind: matchmaker
  p.ws.send(casinoRequest(0x12, 8, (w) => w.u32(1)));
  const enter = await nextCasino(p);
  assert.deepEqual([enter.ch, enter.type, enter.action, enter.r.u8()], [0x12, 1, 8, 0]);
  return { c: p, name, id: Number(profile.id), profile };
}

/** A casino reply on the casino channel: [u8 type][u8 action] + fields. */
function casinoReply(type, action, fill = (w) => w, ch = 0x11) {
  return Buffer.from(frame(ch, fill(new Writer().u8(type).u8(action)).bytes()));
}

/**
 * Quick Match for two signed-in players, in the order the engine sees it:
 *  - the first request waits: RESPONSE 9 lists nobody (n=0);
 *  - the second request's RESPONSE lists the first player (n=1), and the first player gets
 *    EVENT CASINO_MATCHMAKING naming the second ("Players in Match");
 *  - each member of the full group gets EVENT CASINO_ENTER for the other, then
 *    REQUEST LOCK_PLAYER_IN_GAME ("Game starting");
 *  - each player answers the lock with RESPONSE 29, and only then gets EVENT CASINO_MATCHED_UP.
 * With `silent` set to a player's index, that player never answers the lock and the match is
 * sent after the server's lock timeout; `wait` bounds how long to wait for MATCHED_UP.
 * Returns the game room.
 */
async function matchUp(players, { silent = -1, wait = 2000 } = {}) {
  const [a, b] = players;
  a.c.ws.send(casinoRequest(0x12, 9, (w) => w.str('PLUMBER').str('FALSE')));
  let r = await nextCasino(a.c);
  assert.deepEqual([r.ch, r.type, r.action], [0x12, 1, 9]);
  assert.deepEqual([r.r.u8(), r.r.u32(), r.r.u8()], [0, 0, 0]);   // result, value, nobody waiting

  b.c.ws.send(casinoRequest(0x12, 9, (w) => w.str('PLUMBER').str('FALSE')));
  r = await nextCasino(b.c);
  assert.deepEqual([r.ch, r.type, r.action], [0x12, 1, 9]);
  assert.deepEqual([r.r.u8(), r.r.u32(), r.r.u8()], [0, 0, 1]);   // result, value, one player waiting
  const waiting = r.r.kv();
  assert.deepEqual([waiting.id, waiting.name], [String(a.id), a.name]);

  r = await nextCasino(a.c);                                       // "Players in Match" gets b
  assert.deepEqual([r.ch, r.type, r.action], [0x12, 2, 9]);
  assert.equal(r.r.str(), String(b.id));
  assert.equal(r.r.kv().name, b.name);

  for (const [p, other] of [[a, b], [b, a]]) {                     // introductions, then the lock
    const enter = await nextCasino(p.c);
    assert.deepEqual([enter.ch, enter.type, enter.action], [0x12, 2, 8]);
    assert.equal(enter.r.str(), String(other.id));
    assert.equal(enter.r.kv().name, other.name);
  }
  for (const [i, p] of players.entries()) {
    const lock = await nextCasino(p.c);
    assert.deepEqual([lock.ch, lock.type, lock.action, lock.r.u32()], [0x11, 0, 29, 0]);
    if (i !== silent) p.c.ws.send(casinoReply(1, 29, (w) => w.u8(0).u32(0)));
  }

  const rooms = [];
  for (const p of players) {
    const ev = await nextCasino(p.c, wait);
    assert.deepEqual([ev.ch, ev.type, ev.action], [0x12, 2, 31]);  // EVENT CASINO_MATCHED_UP
    ev.r.str();                                                    // casino name
    const room = ev.r.str(), address = ev.r.str();
    assert.ok(address.startsWith('naf://') && address.includes('/' + room + '?hosted='), address);
    rooms.push(room);
  }
  assert.equal(rooms[0], rooms[1]);
  return rooms[0];
}

/** Both players move into their matched game room; the room greets each only once both are in. */
async function enterGameRoom(players, room) {
  const [a, b] = players.map((p) => p.c);
  const joined = async (c) => { let m; do { m = await c.next(); } while (m.t !== 'joined'); return m; };
  a.send({ t: 'move', room });
  assert.equal((await joined(a)).t, 'joined');
  await a.noBinary();
  b.send({ t: 'move', room });
  assert.equal((await joined(b)).t, 'joined');
  assert.deepEqual(await nextBinary(b), ROOM_GREETING);
  assert.deepEqual(await nextBinary(a), ROOM_GREETING);
}

test('casino: greeting, login, enter, quick match pairs two players into a game room', async () => {
  const players = [await signIn('Ann'), await signIn('Ben')];
  const room = await matchUp(players);
  await enterGameRoom(players, room);
  for (const p of players) p.c.ws.close();
});

test('casino: MATCHED_UP is still sent after the lock timeout when one player never answers', async () => {
  const players = [await signIn('Gus'), await signIn('Hal')];
  const t0 = Date.now();
  await matchUp(players, { silent: 1, wait: 4500 });
  const waited = Date.now() - t0;
  assert.ok(waited >= 2500 && waited < 4500, `matched after ${waited} ms`);   // server LOCK_TIMEOUT_MS is 3 s
  for (const p of players) p.c.ws.close();
});

test('game room: handshake, seating and forwarding', async () => {
  const players = [await signIn('Cy'), await signIn('Dee')];
  const [a, b] = players.map((p) => p.c);
  const [idA, idB] = players.map((p) => p.id);
  const room = await matchUp(players);
  await enterGameRoom(players, room);

  // transport hello and keep-alive
  a.ws.send(Buffer.from(frame(2, Uint8Array.of(0))));
  assert.deepEqual(await nextBinary(a), Buffer.from(frame(2, Uint8Array.of(0x20))));
  a.ws.send(Buffer.from(frame(1, Uint8Array.of(7))));
  assert.deepEqual(await nextBinary(a), Buffer.from(frame(2, Uint8Array.of(7))));

  // seating: A takes a seat first, then B; each seated player is announced to everyone
  a.ws.send(tableMsg(REQUEST, SEAT_TAKE, (w) => w.u32(idA)));
  assert.deepEqual(await nextBinary(a), tableMsg(RESPONSE, SEAT_TAKE, (w) => w.u8(0).u32(idA)));
  assert.deepEqual(await nextBinary(a), tableMsg(EVENT, SEAT_TAKE, (w) => w.str(String(idA))));
  assert.deepEqual(await nextBinary(b), tableMsg(EVENT, SEAT_TAKE, (w) => w.str(String(idA))));

  b.ws.send(tableMsg(REQUEST, SEAT_TAKE, (w) => w.u32(idB)));
  assert.deepEqual(await nextBinary(b), tableMsg(RESPONSE, SEAT_TAKE, (w) => w.u8(0).u32(idB)));
  assert.deepEqual(await nextBinary(b), tableMsg(EVENT, SEAT_TAKE, (w) => w.str(String(idA))));  // already seated
  assert.deepEqual(await nextBinary(b), tableMsg(EVENT, SEAT_TAKE, (w) => w.str(String(idB))));
  assert.deepEqual(await nextBinary(a), tableMsg(EVENT, SEAT_TAKE, (w) => w.str(String(idB))));

  // match traffic on another channel is forwarded unchanged, never echoed to the sender
  const data = Buffer.from(frame(0x20, Uint8Array.of(1, 2, 3)));
  a.ws.send(data);
  assert.deepEqual(await nextBinary(b), data);
  await a.noBinary();

  // A leaves: B learns that A's seat is gone
  a.ws.close();
  assert.deepEqual(await nextBinary(b), tableMsg(EVENT, SEAT_LEAVE, (w) => w.str(String(idA))));
  b.ws.close();
});
