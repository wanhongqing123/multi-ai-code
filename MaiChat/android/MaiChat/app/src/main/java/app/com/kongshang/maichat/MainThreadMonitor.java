package com.kongshang.maichat;

import android.os.Debug;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.os.SystemClock;
import android.view.FrameMetrics;
import android.view.Window;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.function.Consumer;

/** Bounded diagnostics written by a background thread, including a stalled main-thread stack. */
final class MainThreadMonitor {
    private final HandlerThread thread = new HandlerThread("MaiChat-diagnostics");
    private final Handler worker;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final Window window;
    private final File directory;
    private volatile boolean foreground = true, closed, pending;
    private volatile long pulse;
    private final java.util.concurrent.atomic.AtomicLong pulseGeneration = new java.util.concurrent.atomic.AtomicLong();
    private volatile String context = "startup";
    private long lastStall, lastMemory;
    private final Window.OnFrameMetricsAvailableListener frames = (ignored, metrics, dropped) -> {
        long total = metrics.getMetric(FrameMetrics.TOTAL_DURATION) / 1_000_000;
        if (!closed && foreground && total >= 32) write("frame totalMs=" + total
            + " layoutMs=" + metrics.getMetric(FrameMetrics.LAYOUT_MEASURE_DURATION) / 1_000_000
            + " drawMs=" + metrics.getMetric(FrameMetrics.DRAW_DURATION) / 1_000_000 + " dropped=" + dropped);
    };
    MainThreadMonitor(Window window, File directory) {
        this.window = window; this.directory = directory;
        thread.start(); worker = new Handler(thread.getLooper());
        window.addOnFrameMetricsAvailableListener(frames, worker);
        worker.post(tick);
    }
    void record(String event) { if (!closed) worker.post(() -> write(event)); }
    void context(String value) { context = value; }
    void foreground(boolean value) {
        foreground = value; pending = false; pulseGeneration.incrementAndGet();
        worker.removeCallbacks(tick); worker.post(tick);
    }
    private final Runnable tick = new Runnable() {
        @Override public void run() {
            if (closed) return;
            long now = SystemClock.uptimeMillis();
            if (foreground) {
                if (!pending) {
                    pulse = now; pending = true; long generation = pulseGeneration.incrementAndGet();
                    main.post(() -> { if (pulseGeneration.get() == generation) pending = false; });
                } else if (now - pulse >= 500 && now - lastStall >= 2000) {
                    lastStall = now;
                    StringBuilder stack = new StringBuilder("main-stall delayMs=" + (now - pulse));
                    for (StackTraceElement item : Looper.getMainLooper().getThread().getStackTrace()) stack.append("\n  at ").append(item);
                    write(stack.toString());
                }
                if (now - lastMemory >= 30000) {
                    lastMemory = now; Debug.MemoryInfo info = new Debug.MemoryInfo(); Debug.getMemoryInfo(info);
                    write("memory pssKiB=" + info.getTotalPss() + " javaBytes="
                        + (Runtime.getRuntime().totalMemory() - Runtime.getRuntime().freeMemory())
                        + " nativeBytes=" + Debug.getNativeHeapAllocatedSize());
                }
            }
            worker.postDelayed(this, foreground ? 100 : 2000);
        }
    };
    private void write(String event) {
        try {
            directory.mkdirs(); File log = new File(directory, "ui.log");
            if (log.length() > 2 * 1024 * 1024) {
                File previous = new File(directory, "ui.previous.log"); previous.delete(); log.renameTo(previous);
            }
            try (FileOutputStream out = new FileOutputStream(log, true)) {
                out.write((System.currentTimeMillis() + " " + context + " " + event + "\n").getBytes(StandardCharsets.UTF_8));
            }
        } catch (Exception ignored) { }
    }
    void export(File report, String header, Consumer<File> success, Runnable failure) {
        worker.post(() -> {
            try {
                report.getParentFile().mkdirs();
                try (FileOutputStream out = new FileOutputStream(report)) {
                    out.write(header.getBytes(StandardCharsets.UTF_8));
                    for (String name : new String[]{"ui.previous.log", "ui.log"}) {
                        File source = new File(directory, name);
                        if (!source.isFile()) continue;
                        out.write(("\n--- " + name + " ---\n").getBytes(StandardCharsets.UTF_8));
                        try (FileInputStream in = new FileInputStream(source)) {
                            byte[] buffer = new byte[8192]; int count;
                            while ((count = in.read(buffer)) >= 0) out.write(buffer, 0, count);
                        }
                    }
                }
                main.post(() -> { if (!closed) success.accept(report); });
            } catch (Exception error) { main.post(() -> { if (!closed) failure.run(); }); }
        });
    }
    void close() {
        closed = true; window.removeOnFrameMetricsAvailableListener(frames);
        worker.removeCallbacks(tick); thread.quitSafely();
    }
}
