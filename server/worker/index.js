// Cloudflare deployment: the Worker routes /ws to a single Durable Object instance that
// runs the same Relay as the Node server. One instance gives every room the ordered,
// single-writer semantics the game needs and is plenty for hobby-scale traffic.
import { Relay, MAX_PACKET } from '../src/relay.js';

export class RelayRoom {
  constructor(state, env) {
    this.state = state;
    this.relay = new Relay();
    this.reaper = null;
  }

  async fetch(request) {
    const url = new URL(request.url);
    if (url.pathname === '/status') return Response.json(this.relay.status());
    if (request.headers.get('Upgrade') !== 'websocket') return new Response('expected websocket', { status: 426 });

    const [client, server] = Object.values(new WebSocketPair());
    server.accept();
    const h = this.relay.connect(
      (data) => { try { server.send(data); } catch { /* closed */ } },
      (code, reason) => { try { server.close(code, reason); } catch { /* closed */ } },
    );
    server.addEventListener('message', (ev) => {
      if (typeof ev.data === 'string') h.onText(ev.data);
      else if (ev.data.byteLength <= MAX_PACKET + 1024) h.onBinary(new Uint8Array(ev.data));
    });
    server.addEventListener('close', h.onClose);
    server.addEventListener('error', h.onClose);
    if (!this.reaper) this.reaper = setInterval(() => this.relay.reapIdle(60_000), 10_000);
    return new Response(null, { status: 101, webSocket: client });
  }
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (url.pathname === '/ws' || url.pathname === '/status') {
      const stub = env.RELAY.get(env.RELAY.idFromName('main'));
      return stub.fetch(request);
    }
    return new Response('Reset Generation Revived relay. Connect a game client to wss://<host>/ws\n');
  },
};
