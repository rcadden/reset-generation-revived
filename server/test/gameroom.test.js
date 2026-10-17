// Unit tests for the game-room bot (no sockets): packets are recorded by a fake send().
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { GameRoom, CH_TABLE, SEAT_TAKE, SEAT_LEAVE } from '../src/gameroom.js';
import { Writer, frame } from '../src/casino.js';

const ROOM_GREETING = Buffer.from('02000200050001204e0000', 'hex');   // transport hello + keep-alive 20000
const REQUEST = 0, RESPONSE = 1, EVENT = 2;

/** A room of `expected` players with a recording send(). */
function setup(expected) {
  const sent = [];   // [client, Buffer]
  const room = new GameRoom({ name: 'QM_1', expected, send: (client, bytes) => sent.push([client, Buffer.from(bytes)]) });
  /** Returns (and forgets) the packets sent to one client. */
  const drain = (c) => {
    const out = [];
    for (let i = 0; i < sent.length;) {
      if (sent[i][0] === c) out.push(sent.splice(i, 1)[0][1]); else i++;
    }
    return out;
  };
  return { room, sent, drain };
}

/** A table-channel message: [u8 type][u8 action] + fields. */
function table(type, action, fill = (w) => w) {
  return Buffer.from(frame(CH_TABLE, fill(new Writer().u8(type).u8(action)).bytes()));
}

test('nothing is sent until every matched player has joined, then each is greeted once', () => {
  const { room, sent, drain } = setup(2);
  const a = { name: 'A' }, b = { name: 'B' };
  room.join(a);
  assert.equal(sent.length, 0);
  room.join(b);
  assert.deepEqual(drain(a), [ROOM_GREETING]);
  assert.deepEqual(drain(b), [ROOM_GREETING]);
  room.start();                                          // already started: no second greeting
  assert.equal(sent.length, 0);
  room.close();
});

test('start() greets a lone player; a late joiner is greeted at once', () => {
  const { room, drain } = setup(2);
  const a = { name: 'A' }, b = { name: 'B' };
  room.join(a);
  room.start();
  assert.deepEqual(drain(a), [ROOM_GREETING]);
  room.join(b);
  assert.deepEqual(drain(b), [ROOM_GREETING]);
  assert.deepEqual(drain(a), []);                        // A is not greeted twice
  room.close();
});

test('transport hello and ping are answered', () => {
  const { room, drain } = setup(1);
  const a = { name: 'A' };
  room.join(a);
  drain(a);
  room.packet(a, frame(2, Uint8Array.of(0)));
  assert.deepEqual(drain(a), [Buffer.from(frame(2, Uint8Array.of(0x20)))]);
  room.packet(a, frame(1, Uint8Array.of(7)));
  assert.deepEqual(drain(a), [Buffer.from(frame(2, Uint8Array.of(7)))]);
  room.close();
});

test('seating: the newcomer learns about seated players, everyone learns about the newcomer', () => {
  const { room, drain } = setup(2);
  const a = { name: 'A' }, b = { name: 'B' };
  room.join(a);
  room.join(b);
  drain(a);
  drain(b);

  room.packet(a, table(REQUEST, SEAT_TAKE, (w) => w.u32(1)));
  assert.deepEqual(drain(a), [
    table(RESPONSE, SEAT_TAKE, (w) => w.u8(0).u32(1)),
    table(EVENT, SEAT_TAKE, (w) => w.str('1')),
  ]);
  assert.deepEqual(drain(b), [table(EVENT, SEAT_TAKE, (w) => w.str('1'))]);

  room.packet(b, table(REQUEST, SEAT_TAKE, (w) => w.u32(2)));
  assert.deepEqual(drain(b), [
    table(RESPONSE, SEAT_TAKE, (w) => w.u8(0).u32(2)),
    table(EVENT, SEAT_TAKE, (w) => w.str('1')),   // already seated
    table(EVENT, SEAT_TAKE, (w) => w.str('2')),
  ]);
  assert.deepEqual(drain(a), [table(EVENT, SEAT_TAKE, (w) => w.str('2'))]);

  room.leave(a);                                         // a seated player leaving is announced
  assert.deepEqual(drain(b), [table(EVENT, SEAT_LEAVE, (w) => w.str('1'))]);
  room.close();
});

test('other channels are forwarded to the other players unchanged, not echoed', () => {
  const { room, drain } = setup(2);
  const a = { name: 'A' }, b = { name: 'B' };
  room.join(a);
  room.join(b);
  drain(a);
  drain(b);
  const data = Buffer.from(frame(0x20, Uint8Array.of(1, 2, 3)));
  room.packet(a, data);
  assert.deepEqual(drain(b), [data]);
  assert.deepEqual(drain(a), []);
  room.close();
});
