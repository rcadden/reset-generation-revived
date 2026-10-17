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

/** A test client that queues every message it receives. */
async function client(name) {
  const ws = new WebSocket(url);
  const queue = [];
  const waiters = [];
  ws.on('message', (data, isBinary) => {
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
