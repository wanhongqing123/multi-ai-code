package com.kongshang.maichat;

import android.content.Context;
import android.os.Handler;
import android.os.Looper;
import android.text.Spanned;
import android.util.LruCache;
import android.widget.TextView;
import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.function.BooleanSupplier;
import io.noties.markwon.Markwon;
import io.noties.markwon.ext.tables.TablePlugin;
import io.noties.markwon.ext.tables.TableAwareMovementMethod;
import io.noties.markwon.ext.strikethrough.StrikethroughPlugin;
import io.noties.markwon.ext.tasklist.TaskListPlugin;
import io.noties.markwon.linkify.LinkifyPlugin;
import io.noties.markwon.movement.MovementMethodPlugin;

/** CommonMark/GFM parsing is serialized off the UI thread; only applying spans touches views. */
public final class MarkdownRenderer {
    private static final ExecutorService worker = Executors.newSingleThreadExecutor();
    private static final Handler main = new Handler(Looper.getMainLooper());
    private static volatile Markwon markwon;
    private static final LruCache<String, Spanned> cache = new LruCache<String, Spanned>(2 * 1024 * 1024) {
        @Override protected int sizeOf(String key, Spanned value) { return Math.max(1, (key.length() + value.length()) * 2); }
    };
    private MarkdownRenderer() { }
    public static synchronized void initialize(Context context) {
        if (markwon == null) markwon = Markwon.builder(context.getApplicationContext())
            .usePlugin(TablePlugin.create(context.getApplicationContext()))
            .usePlugin(StrikethroughPlugin.create())
            .usePlugin(TaskListPlugin.create(context.getApplicationContext()))
            .usePlugin(LinkifyPlugin.create())
            .usePlugin(MovementMethodPlugin.create(TableAwareMovementMethod.create()))
            .build();
    }
    private static String source(String value) { return value == null ? "" : value.replace("\r\n", "\n"); }
    private static Spanned prepare(String value) {
        if (Looper.myLooper() == Looper.getMainLooper()) throw new IllegalStateException("Markdown parsing on UI thread");
        String key = source(value);
        Spanned ready = cache.get(key);
        if (ready == null) {
            try { ready = markwon.toMarkdown(key); }
            catch (RuntimeException | StackOverflowError error) {
                android.util.Log.w("MaiChat.markdown", "Could not render message; showing plain text", error);
                ready = new android.text.SpannedString(key);
            }
            cache.put(key, ready);
        }
        return ready;
    }
    public static void prepare(List<String> sources, BooleanSupplier current, Runnable completion) {
        worker.execute(() -> {
            for (String value : sources) { if (!current.getAsBoolean()) return; prepare(value); }
            main.post(() -> { if (current.getAsBoolean()) completion.run(); });
        });
    }
    public static void bind(TextView view, String value) {
        String key = source(value);
        Object token = new Object();
        view.setTag(token);
        Spanned ready = cache.get(key);
        if (ready != null) { markwon.setParsedMarkdown(view, ready); return; }
        // A recycled row may miss the bounded cache. Never parse synchronously as a fallback.
        view.setText(value);
        worker.execute(() -> {
            Spanned parsed = prepare(key);
            main.post(() -> { if (view.getTag() == token) markwon.setParsedMarkdown(view, parsed); });
        });
    }
}
