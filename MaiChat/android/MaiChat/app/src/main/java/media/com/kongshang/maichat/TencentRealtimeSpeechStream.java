// 文件作用：按实时节奏读取 AAC，并通过 WebSocket 接收腾讯语音识别结果。
// 主要入口：dispatch、receive、pump、finish。
package com.kongshang.maichat;

import java.io.File;
import java.util.UUID;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;
import org.json.JSONObject;

/** Streams the same compressed AAC file retained for fallback, without a second microphone. */
final class TencentRealtimeSpeechStream {
    interface Listener {
        void partial(String text);
        void finished(String text);
        default void diagnostic(String event) {}
    }
    private interface SocketFactory {
        CurlRealtimeSpeechWebSocket open(String voiceId, CurlRealtimeSpeechWebSocket.Listener listener)
            throws Exception;
    }
    private final ScheduledExecutorService worker = Executors.newSingleThreadScheduledExecutor();
    private final String voiceId = UUID.randomUUID().toString();
    private final RealtimeSpeechProtocol transcript = new RealtimeSpeechProtocol();
    private final Listener listener;
    private final File audio;
    private final SocketFactory socketFactory;
    private CurlRealtimeSpeechWebSocket socket;
    private AdtsFrameReader reader;
    private ScheduledFuture<?> pumping;
    private boolean ready, audioEnded, endSent, finished;
    private long readyAt;
    private String previousText = "";

    TencentRealtimeSpeechStream(File audio, String appId, String secretId, String secretKey, Listener listener) {
        this(audio, listener, (voiceId, callback) -> {
            String url = RealtimeSpeechProtocol.signedUrl(appId, secretId, secretKey, voiceId,
                System.currentTimeMillis() / 1000,
                new java.security.SecureRandom().nextInt(Integer.MAX_VALUE - 1) + 1);
            return new CurlRealtimeSpeechWebSocket(url, callback);
        });
    }

    // 本地 WSS 集成测试的连接入口；不读取付费服务凭据。
    TencentRealtimeSpeechStream(File audio, String url, byte[] caPem, MaiChatAuthTransport.ProxyRoute proxy,
                                Listener listener) {
        this(audio, listener, (voiceId, callback) ->
            new CurlRealtimeSpeechWebSocket(url, caPem, proxy, callback));
    }

    private TencentRealtimeSpeechStream(File audio, Listener listener, SocketFactory socketFactory) {
        this.audio = audio; this.listener = listener; this.socketFactory = socketFactory;
        worker.execute(() -> {
            try {
                listener.diagnostic("asr-start");
                socket = socketFactory.open(voiceId, new CurlRealtimeSpeechWebSocket.Listener() {
                    @Override public void message(String text) { dispatch(() -> receive(text)); }
                    @Override public void failed(int code) {
                        dispatch(() -> {
                            listener.diagnostic("asr-transport-failed curl=" + code);
                            finish(null);
                        });
                    }
                    @Override public void closed() {
                        dispatch(() -> {
                            if (!finished)
                                finish(null);
                        });
                    }
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
            if (message.optInt("code", -1) != 0 || !voiceId.equals(message.optString("voice_id", voiceId))) {
                listener.diagnostic("asr-service-failed code=" + message.optInt("code", -1));
                finish(null);
                return;
            }
            if (!ready) {
                listener.diagnostic("asr-ready");
                ready = true; readyAt = System.nanoTime(); reader = new AdtsFrameReader(audio);
                pumping = worker.scheduleAtFixedRate(this::pump, 0, 200, TimeUnit.MILLISECONDS);
            }
            transcript.accept(message);
            String value = transcript.text();
            if (!value.equals(previousText)) { previousText = value; listener.partial(value); }
            if (transcript.complete()) {
                listener.diagnostic("asr-server-final chars=" + value.length());
                finish(endSent ? value : null);
            }
        } catch (Exception error) { finish(null); }
    }
    private void pump() {
        if (finished || !ready || endSent) return;
        try {
            // TLS 握手即使较慢，AAC 样本仍按实时节奏发送，不能瞬间倾倒积压音频。
            long budget = ((System.nanoTime() - readyAt) / 1_000_000 + 200) * 16;
            byte[] packet = reader.readUntilSample(budget);
            if (packet.length > 0 && !socket.sendBinary(packet)) {
                finish(null);
                return;
            }
            if (audioEnded && packet.length == 0 && !reader.atEnd() && budget - reader.samples() > 8192) {
                finish(null);
                return;
            }
            if (audioEnded && reader.atEnd()) {
                endSent = true;
                listener.diagnostic("asr-end-sent samples=" + reader.samples());
                if (!socket.sendText("{\"type\":\"end\"}")) {
                    finish(null);
                    return;
                }
                pumping.cancel(false);
                worker.schedule(() -> {
                    listener.diagnostic("asr-end-timeout");
                    finish(transcript.text().isEmpty() ? null : transcript.text());
                }, 1, TimeUnit.SECONDS);
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
