// 文件作用：按实时节奏读取 AAC，直连腾讯 WebSocket 并接收 V2 语音识别结果。
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
        void failed();
        default void diagnostic(String event) {}
    }
    private interface SocketFactory {
        CurlRealtimeSpeechWebSocket open(String voiceId, CurlRealtimeSpeechWebSocket.Listener listener)
            throws Exception;
    }
    private final ScheduledExecutorService worker = Executors.newSingleThreadScheduledExecutor();
    private final String voiceId;
    private final RealtimeSpeechProtocol transcript;
    private final Listener listener;
    private final File audio;
    private final SocketFactory socketFactory;
    private CurlRealtimeSpeechWebSocket socket;
    private AdtsFrameReader reader;
    private ScheduledFuture<?> pumping;
    private boolean ready, audioEnded, endSent, finished;
    private long readyAt;
    private String previousText = "";

    // 本地 WSS 集成测试的连接入口；不读取付费服务凭据。
    TencentRealtimeSpeechStream(File audio, String url, byte[] caPem, MaiChatAuthTransport.ProxyRoute proxy,
                                Listener listener) {
        this(audio, url, "", caPem, proxy, listener);
    }

    TencentRealtimeSpeechStream(File audio, String url, String token, byte[] caPem,
                                MaiChatAuthTransport.ProxyRoute proxy, Listener listener) {
        this(audio, UUID.randomUUID().toString(), listener, (voiceId, callback) ->
            new CurlRealtimeSpeechWebSocket(url, token, caPem, proxy, callback));
    }

    TencentRealtimeSpeechStream(File audio, MaiChatAuthClient.RealtimeAsrTicket ticket,
                                Listener listener) {
        this(audio, ticket.voiceId, listener, (voiceId, callback) ->
            new CurlRealtimeSpeechWebSocket(ticket.url, ticket.token, callback));
    }

    private TencentRealtimeSpeechStream(File audio, String voiceId, Listener listener,
                                        SocketFactory socketFactory) {
        this.audio = audio; this.voiceId = voiceId;
        this.transcript = new RealtimeSpeechProtocol(voiceId);
        this.listener = listener; this.socketFactory = socketFactory;
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
            if (!transcript.accept(message)) return;
            if (transcript.failed()) {
                listener.diagnostic("asr-service-failed code=" + message.optInt("code", -1));
                finish(null);
                return;
            }
            if (!ready) {
                listener.diagnostic("asr-ready");
                ready = true; readyAt = System.nanoTime(); reader = new AdtsFrameReader(audio);
                pumping = worker.scheduleAtFixedRate(this::pump, 0, 200, TimeUnit.MILLISECONDS);
            }
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
                    // V2 可在结束时修正全文；没有 final 就不能把中间态当成功发送。
                    listener.diagnostic("asr-end-timeout");
                    finish(null);
                }, 10, TimeUnit.SECONDS);
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
        if (notify) {
            if (text == null) listener.failed();
            else listener.finished(text);
        }
        worker.shutdownNow();
    }
}
