// Game server role: channel-0x16 messages are reassembled from MT packets, routed to the
// serving process tagged with a player id, and its replies are split into MT packets.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { GameRoom, CH_GAME, CH_TABLE, SEAT_TAKE, mtPackets, mtMessages, MT_PIECE } from '../src/gameroom.js';
import { Writer, frame } from '../src/casino.js';

function setup() {
  const sent = [], ctl = [], toServer = [];
  const room = new GameRoom({ name: 'QM_1', expected: 1, send: (c, b) => sent.push([c, Buffer.from(b)]) });
  room.attachServer({ ctl: (m) => ctl.push(m), send: (b) => toServer.push(Buffer.from(b)) });
  return { room, sent, ctl, toServer };
}

test('mtPackets / mtMessages round-trip a large message', () => {
  const data = Uint8Array.from({ length: 5000 }, (_, i) => i & 255);
  const pkts = mtPackets(CH_GAME, data);
  assert.equal(pkts.length, Math.ceil(5001 / MT_PIECE));
  const state = {}, out = [];
  for (const p of pkts) out.push(...mtMessages(state, p));
  assert.equal(out.length, 1);
  assert.equal(out[0].ch, CH_GAME);
  assert.deepEqual(Buffer.from(out[0].data), Buffer.from(data));
  assert.equal(mtPackets(CH_GAME, Uint8Array.of(1, 2, 3)).length, 1);   // small message: one packet, piece 0
});

test('the server hears about a player once it is seated; messages are routed both ways', () => {
  const { room, sent, ctl, toServer } = setup();
  const a = { name: 'A' };
  room.join(a);
  assert.deepEqual(ctl, []);
  room.packet(a, frame(CH_TABLE, new Writer().u8(0).u8(13).u32(7).bytes()));   // REQUEST SEAT_TAKE
  assert.deepEqual(ctl, [{ t: 'player_joined', pid: 1, name: 'A' }]);
  assert.equal(SEAT_TAKE, 13);
  sent.length = 0;
  room.packet(a, mtPackets(CH_GAME, Uint8Array.of(9, 8, 7))[0]);
  assert.deepEqual([...toServer[0]], [1, 0, 9, 8, 7]);                          // [u16 player id][message]
  const big = new Uint8Array(2000).fill(5);
  room.fromServer(Uint8Array.from([1, 0, ...big]));
  const state = {}, got = [];
  for (const [, b] of sent) got.push(...mtMessages(state, b));
  assert.equal(got.length, 1);
  assert.equal(got[0].data.length, 2000);
  room.leave(a);
  assert.deepEqual(ctl[1], { t: 'player_left', pid: 1 });
  room.close();
});
