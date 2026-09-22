package com.kongshang.maichat;

import java.io.File;
import java.util.UUID;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;
import okhttp3.OkHttpClient;
import okhttp3.Request;
import okhttp3.Response;
import okhttp3.WebSocket;
import okhttp3.WebSocketListener;
import okio.ByteString;
import org.json.JSONObject;

/** Streams the same compressed AAC file retained for fallback, without a second microphone. */
final class TencentRealtimeSpeechStream {
    interface Listener { void partial(String text); void finished(String text); default void diagnostic(String event) { } }
    private static final OkHttpClient HTTP = new OkHttpClient.Builder()
        .connectTimeout(5, TimeUnit.SECONDS).writeTimeout(5, TimeUnit.SECONDS).readTimeout(0, TimeUnit.MILLISECONDS).build();
    private final ScheduledExecutorService worker = Executors.newSingleThreadScheduledExecutor();
    private final String voiceId = UUID.randomUUID().toString();
    private final RealtimeSpeechProtocol transcript = new RealtimeSpeechProtocol();
    private final Listener listener;
    private final File audio;
    private WebSocket socket;
    private AdtsFrameReader reader;
    private ScheduledFuture<?> pumping;
    private boolean ready, audioEnded, endSent, finished;
    private long readyAt;
    private String previousText = "";

    TencentRealtimeSpeechStream(File audio, String appId, String secretId, String secretKey, Listener listener) {
        this.audio = audio; this.listener = listener;
        worker.execute(() -> {
            try {
                String url = RealtimeSpeechProtocol.signedUrl(appId, secretId, secretKey, voiceId,
                    System.currentTimeMillis() / 1000, new java.security.SecureRandom().nextInt(Integer.MAX_VALUE - 1) + 1);
                listener.diagnostic("asr-start");
                socket = HTTP.newWebSocket(new Request.Builder().url(url).build(), new WebSocketListener() {
                    @Override public void onMessage(WebSocket socket, String text) { dispatch(() -> receive(text)); }
                    @Override public void onFailure(WebSocket socket, Throwable error, Response response) { dispatch(() -> { listener.diagnostic("asr-transport-failed type=" + error.getClass().getSimpleName()); finish(null); }); }
                    @Override public void onClosed(WebSocket socket, int code, String reason) { dispatch(() -> { if (!finished) finish(null); }); }
                });
                worker.schedule(() -> { if (!ready) finish(null); }, 6, TimeUnit.SECONDS);
            } catch (Exception error) { finish(null); }
        });
    }
    private void dispatch(Runnable action) {
        if (worker.isShutdown()) return;
        try { worker.execute(action); } catch (java.util.concurrent.RejectedExecutionException ignored) { }
    }
    private void receive(String text) {
        if (finished) return;
        try {
            JSONObject message = new JSONObject(text);
            if (message.optInt("code", -1) != 0 || !voiceId.equals(message.optString("voice_id", voiceId))) { listener.diagnostic("asr-service-failed code=" + message.optInt("code", -1)); finish(null); return; }
            if (!ready) {
                listener.diagnostic("asr-ready");
                ready = true; readyAt = System.nanoTime(); reader = new AdtsFrameReader(audio);
                pumping = worker.scheduleAtFixedRate(this::pump, 0, 200, TimeUnit.MILLISECONDS);
            }
            transcript.accept(message);
            String value = transcript.text();
            if (!value.equals(previousText)) { previousText = value; listener.partial(value); }
            if (transcript.complete()) { listener.diagnostic("asr-server-final chars=" + value.length()); finish(endSent ? value : null); }
        } catch (Exception error) { finish(null); }
    }
    private void pump() {
        if (finished || !ready || endSent) return;
        try {
            // Keep AAC sample time at the realtime rate even after a slow TLS handshake.
            long budget = ((System.nanoTime() - readyAt) / 1_000_000 + 200) * 16;
            byte[] packet = reader.readUntilSample(budget);
            if (socket.queueSize() > 128 * 1024 || packet.length > 0 && !socket.send(ByteString.of(packet))) { finish(null); return; }
            if (audioEnded && packet.length == 0 && !reader.atEnd() && budget - reader.samples() > 8192) { finish(null); return; }
            if (audioEnded && reader.atEnd()) {
                endSent = true;
                listener.diagnostic("asr-end-sent samples=" + reader.samples());
                socket.send("{\"type\":\"end\"}");
                pumping.cancel(false);
                worker.schedule(() -> { listener.diagnostic("asr-end-timeout"); finish(transcript.text().isEmpty() ? null : transcript.text()); }, 1, TimeUnit.SECONDS);
            }
        } catch (Exception error) { finish(null); }
    }
    void endAudio() { dispatch(() -> { audioEnded = true; pump(); }); }
    void cancel() { dispatch(() -> finish(null, false)); }
    private void finish(String text) { finish(text, true); }
    private void finish(String text, boolean notify) {
        if (finished) return;
        finished = true;
        if (pumping != null) pumping.cancel(false);
        if (socket != null) socket.cancel();
        if (reader != null) try { reader.close(); } catch (Exception ignored) { }
        if (notify) listener.finished(text);
        worker.shutdownNow();
    }
}
