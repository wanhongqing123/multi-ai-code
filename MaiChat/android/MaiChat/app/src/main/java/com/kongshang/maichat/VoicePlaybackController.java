package com.kongshang.maichat;

import android.media.MediaPlayer;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import java.util.concurrent.atomic.AtomicLong;

/** Media data-source setup, decoding and teardown never execute on the UI thread. */
final class VoicePlaybackController {
    private final HandlerThread thread = new HandlerThread("MaiChat-playback");
    private final Handler worker;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final AtomicLong generation = new AtomicLong();
    private volatile boolean closed;
    private MediaPlayer player;
    VoicePlaybackController() { thread.start(); worker = new Handler(thread.getLooper()); }
    void play(String path, Runnable completed) {
        long request = generation.incrementAndGet();
        worker.post(() -> {
            if (closed || request != generation.get()) return;
            release();
            try {
                MediaPlayer next = new MediaPlayer(); player = next;
                next.setDataSource(path);
                next.setOnPreparedListener(value -> { if (!closed && request == generation.get()) value.start(); });
                next.setOnCompletionListener(value -> finish(request, completed));
                next.setOnErrorListener((value, what, extra) -> { finish(request, completed); return true; });
                next.prepareAsync();
            } catch (Exception error) { finish(request, completed); }
        });
    }
    private void finish(long request, Runnable completed) {
        if (request != generation.get()) return;
        release(); main.post(() -> { if (!closed && request == generation.get()) completed.run(); });
    }
    void stop() { generation.incrementAndGet(); worker.post(this::release); }
    private void release() {
        if (player == null) return;
        try { player.stop(); } catch (IllegalStateException ignored) { }
        player.release(); player = null;
    }
    void close() { closed = true; generation.incrementAndGet(); worker.post(() -> { release(); thread.quitSafely(); }); }
}
