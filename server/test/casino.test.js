// Unit tests for the casino bot's Quick Match queue (no sockets): packets are recorded by a fake send().
// A pair of queued players is always matched at once over real sockets, so a player can only be
// left waiting in the queue with others when the group size is larger than two.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { Casino, Reader, Writer, frame, parseFrames } from '../src/casino.js';

const CH_MATCHMAKER = 0x12;
const EVENT = 2;

test('a queued player who disconnects is announced with EVENT CASINO_CANCEL_MATCHMAKING to the others', () => {
  const sent = [];   // [client, Buffer]
  const casino = new Casino({ send: (client, bytes) => sent.push([client, Buffer.from(bytes)]) });
  casino.matchSize = 3;                                  // group of three: two players stay queued
  const a = { name: 'A' }, b = { name: 'B' };
  casino.join(a);
  casino.join(b);
  const aId = casino.profile(a).id;

  const requestMatch = (client) => casino.packet(client, frame(CH_MATCHMAKER,
    new Writer().u8(0).u8(9).str('PLUMBER').str('FALSE').bytes()));
  requestMatch(a);
  requestMatch(b);
  sent.length = 0;                                       // forget joins and the matchmaking replies

  casino.leave(a);                                       // a disconnects while queued

  const toB = sent.filter(([c]) => c === b).flatMap(([, bytes]) => parseFrames(bytes));
  assert.equal(toB.length, 1);
  assert.equal(toB[0].ch, CH_MATCHMAKER);
  const r = new Reader(toB[0].data);
  assert.deepEqual([r.u8(), r.u8()], [EVENT, 10]);       // EVENT CASINO_CANCEL_MATCHMAKING
  assert.equal(r.str(), aId);
  assert.equal(sent.filter(([c]) => c === a).length, 0); // the leaving player gets nothing
});
