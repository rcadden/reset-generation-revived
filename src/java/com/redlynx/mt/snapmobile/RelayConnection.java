package com.redlynx.mt.snapmobile;

import java.io.ByteArrayOutputStream;
import java.net.URI;
import java.net.http.HttpClient;
import java.net.http.WebSocket;
import java.nio.ByteBuffer;
import java.time.Duration;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * WebSocket connection to the Reset Generation Revived relay (protocol in docs/MULTIPLAYER.md).
 * Control messages are JSON text frames; game packets are binary frames.
 */
final class RelayConnection implements WebSocket.Listener {
    private static final long REQUEST_TIMEOUT_MS = 10000;
    private static final long PING_INTERVAL_MS = 15000;

    private final SNAPMobileClient client;
    private final LinkedBlockingQueue<Map<String, Object>> replies = new LinkedBlockingQueue<Map<String, Object>>();
    private final StringBuilder text = new StringBuilder();
    private final ByteArrayOutputStream binary = new ByteArrayOutputStream();
    private final AtomicBoolean closed = new AtomicBoolean();
    private final Object sendLock = new Object();
    private volatile WebSocket ws;
    private Thread pinger;

    private RelayConnection(SNAPMobileClient client) {
        this.client = client;
    }

    /** Connects (blocking, 10 s timeout); null if the server cannot be reached. */
    static RelayConnection open(String url, SNAPMobileClient client) {
        RelayConnection c = new RelayConnection(client);
        try {
            HttpClient http = HttpClient.newBuilder().connectTimeout(Duration.ofSeconds(10)).build();
            c.ws = http.newWebSocketBuilder().buildAsync(URI.create(url), c).get(15, TimeUnit.SECONDS);
        } catch (Exception e) {
            SMLog.printf("Relay connect to " + url + " failed: " + e + "\n");
            return null;
        }
        c.startPinger();
        return c;
    }

    boolean isOpen() {
        return !this.closed.get();
    }

    /** Sends a control message and waits for the reply of the given type (or an error). */
    Map<String, Object> request(String json, String replyType) {
        this.replies.clear();
        if (!this.sendText(json)) {
            return null;
        }
        long deadline = System.currentTimeMillis() + REQUEST_TIMEOUT_MS;
        try {
            while (true) {
                long left = deadline - System.currentTimeMillis();
                Map<String, Object> m = left > 0 ? this.replies.poll(left, TimeUnit.MILLISECONDS) : null;
                if (m == null || replyType.equals(m.get("t")) || "error".equals(m.get("t"))) {
                    return m;
                }
            }
        } catch (InterruptedException e) {
            return null;
        }
    }

    boolean sendPacket(byte[] data) {
        return this.send(null, data);
    }

    private boolean sendText(String s) {
        SNAPMobileClient.debug("relay > " + s);
        return this.send(s, null);
    }

    /** java.net.http allows one outstanding send per socket, so sends are serialised. */
    private boolean send(String s, byte[] data) {
        WebSocket w = this.ws;
        if (w == null || this.closed.get()) {
            return false;
        }
        synchronized (this.sendLock) {
            try {
                CompletableFuture<WebSocket> f = s != null ? w.sendText(s, true) : w.sendBinary(ByteBuffer.wrap(data), true);
                f.get(REQUEST_TIMEOUT_MS, TimeUnit.MILLISECONDS);
                return true;
            } catch (Exception e) {
                this.fail("send failed: " + e);
                return false;
            }
        }
    }

    void close() {
        if (this.closed.compareAndSet(false, true)) {
            WebSocket w = this.ws;
            if (w != null) {
                try {
                    w.sendClose(WebSocket.NORMAL_CLOSURE, "bye");
                } catch (Exception e) {
                    w.abort();
                }
            }
            if (this.pinger != null) {
                this.pinger.interrupt();
            }
        }
    }

    private void fail(String why) {
        if (this.closed.compareAndSet(false, true)) {
            WebSocket w = this.ws;
            if (w != null) {
                w.abort();
            }
            if (this.pinger != null) {
                this.pinger.interrupt();
            }
            this.client.onRelayClosed(this, why);
        }
    }

    private void startPinger() {
        this.pinger = new Thread(new Runnable() {
            public void run() {
                while (!closed.get()) {
                    try {
                        Thread.sleep(PING_INTERVAL_MS);
                    } catch (InterruptedException e) {
                        return;
                    }
                    sendText("{\"t\":\"ping\"}");
                }
            }
        }, "RelayPing");
        this.pinger.setDaemon(true);
        this.pinger.start();
    }

    // ------------------------------------------------------------------ WebSocket.Listener

    public void onOpen(WebSocket webSocket) {
        webSocket.request(1);
    }

    public CompletionStage<?> onText(WebSocket webSocket, CharSequence data, boolean last) {
        this.text.append(data);
        if (last) {
            String s = this.text.toString();
            this.text.setLength(0);
            Map<String, Object> m = Json.parseObject(s);
            SNAPMobileClient.debug("relay < " + s);
            if (m != null) {
                Object t = m.get("t");
                if (!"pong".equals(t) && !"peer_joined".equals(t) && !"peer_left".equals(t)) {
                    this.replies.offer(m);
                }
            }
        }
        webSocket.request(1);
        return null;
    }

    public CompletionStage<?> onBinary(WebSocket webSocket, ByteBuffer data, boolean last) {
        byte[] chunk = new byte[data.remaining()];
        data.get(chunk);
        this.binary.write(chunk, 0, chunk.length);
        if (last) {
            byte[] packet = this.binary.toByteArray();
            this.binary.reset();
            this.client.onRelayPacket(packet);
        }
        webSocket.request(1);
        return null;
    }

    public CompletionStage<?> onClose(WebSocket webSocket, int statusCode, String reason) {
        this.fail("closed by server (" + statusCode + " " + reason + ")");
        return null;
    }

    public void onError(WebSocket webSocket, Throwable error) {
        this.fail("error: " + error);
    }
}
