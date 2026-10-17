// Self-hosted relay: `node node/server.js [--port 8787] [--host 0.0.0.0]` (or PORT/HOST env).
// WebSocket endpoint at /ws; GET /status returns JSON counters; GET / is a health check.
import http from 'node:http';
import { WebSocketServer } from 'ws';
import { Relay, MAX_PACKET } from '../src/relay.js';

const arg = (name, fallback) => {
  const i = process.argv.indexOf(`--${name}`);
  return i > 0 ? process.argv[i + 1] : fallback;
};
const PORT = Number(arg('port', process.env.PORT ?? 8787));
const HOST = arg('host', process.env.HOST ?? '0.0.0.0');
const IDLE_MS = 60_000;      // clients ping every 15 s
const quiet = process.argv.includes('--quiet');

const relay = new Relay({
  log: quiet ? () => {} : (s) => console.log(new Date().toISOString(), s),
  logPackets: process.argv.includes('--log-packets'),
});

const server = http.createServer((req, res) => {
  if (req.url === '/status') {
    res.writeHead(200, { 'content-type': 'application/json' });
    return res.end(JSON.stringify(relay.status()));
  }
  res.writeHead(200, { 'content-type': 'text/plain' });
  res.end('Reset Generation Revived relay. Connect a game client to ws://<host>/ws\n');
});

const wss = new WebSocketServer({ server, path: '/ws', maxPayload: MAX_PACKET + 1024 });
wss.on('connection', (ws) => {
  const h = relay.connect(
    (data) => { if (ws.readyState === ws.OPEN) ws.send(data, { binary: typeof data !== 'string' }); },
    (code, reason) => ws.close(code, reason),
  );
  ws.on('message', (data, isBinary) => {
    if (isBinary) h.onBinary(new Uint8Array(data.buffer, data.byteOffset, data.byteLength));
    else h.onText(data.toString('utf8'));
  });
  ws.on('pong', () => { h.client.lastSeen = Date.now(); });
  ws.on('close', h.onClose);
  ws.on('error', h.onClose);
});

const reaper = setInterval(() => relay.reapIdle(IDLE_MS), 10_000);
server.listen(PORT, HOST, () => console.log(`relay listening on ws://${HOST}:${PORT}/ws`));

const stop = () => { clearInterval(reaper); wss.close(); server.close(() => process.exit(0)); };
process.on('SIGINT', stop);
process.on('SIGTERM', stop);
