package com.kongshang.maichat;

import android.media.MediaRecorder;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.os.SystemClock;
import java.io.File;
import java.util.HashSet;
import java.util.Set;
import java.util.concurrent.atomic.AtomicBoolean;

/** One microphone and AAC file feed both realtime transcription and voice fallback. */
final class VoiceRecordingController {
    interface Completion {
        void finished(File file, int seconds);
        void failed();
        default void partial(String text) { }
        default void recognized(String text, File file, int seconds) { finished(file, seconds); }
    }
    private static final class Clip {
        final File file; final Completion completion;
        TencentRealtimeSpeechStream stream;
        String text;
        int seconds;
        boolean released, resultReady, cancelled, delivered;
        Clip(File file, Completion completion) { this.file = file; this.completion = completion; }
    }
    private final HandlerThread thread = new HandlerThread("MaiChat-audio");
    private final Handler worker;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final RemoteIMMediaStore store;
    private final java.util.function.Consumer<String> diagnostic;
    private final AtomicBoolean active = new AtomicBoolean();
    private final Set<Clip> clips = new HashSet<>();
    private volatile boolean closed;
    private MediaRecorder recorder;
    private Clip recording;
    private long started;
    VoiceRecordingController(RemoteIMMediaStore store) { this(store, event -> { }); }
    VoiceRecordingController(RemoteIMMediaStore store, java.util.function.Consumer<String> diagnostic) {
        this.diagnostic = diagnostic;
        this.store = store; thread.start(); worker = new Handler(thread.getLooper());
    }
    void start(Completion callback) {
        if (closed || !active.compareAndSet(false, true)) return;
        worker.post(() -> {
            if (closed) { active.set(false); return; }
            try {
                Clip clip = new Clip(store.createRealtimeVoiceRecordingFile(), callback);
                recording = clip; clips.add(clip);
                recorder = new MediaRecorder();
                recorder.setAudioSource(MediaRecorder.AudioSource.MIC);
                recorder.setOutputFormat(MediaRecorder.OutputFormat.AAC_ADTS);
                recorder.setAudioEncoder(MediaRecorder.AudioEncoder.AAC);
                recorder.setAudioSamplingRate(16000); recorder.setAudioChannels(1); recorder.setAudioEncodingBitRate(32000);
                recorder.setOutputFile(clip.file.getAbsolutePath());
                recorder.prepare(); recorder.start(); started = SystemClock.elapsedRealtime();
                if (!BuildConfig.TENCENT_ASR_APP_ID.isEmpty() && !BuildConfig.TENCENT_ASR_SECRET_ID.isEmpty()
                    && !BuildConfig.TENCENT_ASR_SECRET_KEY.isEmpty()) {
                    clip.stream = new TencentRealtimeSpeechStream(clip.file, BuildConfig.TENCENT_ASR_APP_ID,
                        BuildConfig.TENCENT_ASR_SECRET_ID, BuildConfig.TENCENT_ASR_SECRET_KEY,
                        new TencentRealtimeSpeechStream.Listener() {
                            @Override public void diagnostic(String event) { diagnostic.accept(event); }
                            @Override public void partial(String text) {
                                main.post(() -> { if (!closed && !clip.cancelled) callback.partial(text); });
                            }
                            @Override public void finished(String text) {
                                worker.post(() -> { clip.text = text; clip.resultReady = true; deliver(clip); });
                            }
                        });
                } else clip.resultReady = true;
            } catch (Exception error) {
                finishOnWorker(true);
                main.post(() -> { if (!closed) callback.failed(); });
            }
        });
    }
    void finish(boolean cancel) { if (!closed) worker.post(() -> finishOnWorker(cancel)); }
    private void finishOnWorker(boolean cancel) {
        Clip clip = recording;
        boolean succeeded = recorder != null;
        if (recorder != null) {
            try { recorder.stop(); } catch (RuntimeException error) { succeeded = false; }
            recorder.release(); recorder = null;
        }
        recording = null; active.set(false);
        if (clip == null) return;
        clip.seconds = Math.max(1, (int) ((SystemClock.elapsedRealtime() - started) / 1000));
        clip.released = true;
        if (cancel || closed || !succeeded) {
            clip.cancelled = true;
            if (clip.stream != null) clip.stream.cancel();
            clip.file.delete(); clips.remove(clip);
            if (!cancel && !closed) main.post(clip.completion::failed);
            return;
        }
        if (clip.stream != null) clip.stream.endAudio();
        deliver(clip);
    }
    private void deliver(Clip clip) {
        if (!clip.released || !clip.resultReady || clip.delivered || clip.cancelled || closed) return;
        clip.delivered = true; clips.remove(clip);
        main.post(() -> {
            if (closed || clip.cancelled) return;
            if (clip.text != null && !clip.text.trim().isEmpty()) clip.completion.recognized(clip.text.trim(), clip.file, clip.seconds);
            else clip.completion.finished(clip.file, clip.seconds);
        });
    }
    void close() {
        closed = true;
        worker.post(() -> {
            finishOnWorker(true);
            for (Clip clip : clips) { clip.cancelled = true; if (clip.stream != null) clip.stream.cancel(); clip.file.delete(); }
            clips.clear(); thread.quitSafely();
        });
    }
}
