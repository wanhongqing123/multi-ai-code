package com.kongshang.maichat;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.drawable.BitmapDrawable;
import android.os.Handler;
import android.os.Looper;
import android.util.LruCache;
import android.widget.ImageView;
import java.io.File;
import java.lang.ref.WeakReference;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicLong;

/** Bounded image cache. Metadata checks and decoding both run off the UI thread. */
public final class MessageImageLoader {
    private static final LruCache<String, Bitmap> CACHE = new LruCache<String, Bitmap>(32 * 1024 * 1024) {
        @Override protected int sizeOf(String key, Bitmap bitmap) { return bitmap.getAllocationByteCount(); }
    };
    private static final Map<String, WeakReference<Bitmap>> RECENT = new LinkedHashMap<>();
    private static final Map<String, List<Request>> WAITING = new HashMap<>();
    private static final ExecutorService WORKER = Executors.newFixedThreadPool(2);
    private static final Handler MAIN = new Handler(Looper.getMainLooper());
    private static final AtomicLong IDS = new AtomicLong();
    public interface MissingHandler { void onMissing(); }
    private static final class Tag {
        final long id; final MissingHandler missing;
        Tag(long id, MissingHandler missing) { this.id = id; this.missing = missing; }
    }
    private static final class Request {
        final long id; final WeakReference<ImageView> view;
        Request(long id, ImageView view) { this.id = id; this.view = new WeakReference<>(view); }
    }
    private MessageImageLoader() { }
    private static String logical(String path, int width, int height) { return path + "\0" + width + "x" + height; }
    public static Bitmap cached(String path, int width, int height) {
        synchronized (RECENT) {
            WeakReference<Bitmap> value = RECENT.get(logical(path, width, height));
            Bitmap bitmap = value == null ? null : value.get();
            return bitmap == null || bitmap.isRecycled() ? null : bitmap;
        }
    }
    public static void load(String path, int width, int height, ImageView target, MissingHandler missing) {
        String clean = path == null ? "" : path.trim();
        long id = IDS.incrementAndGet();
        target.setTag(new Tag(id, missing));
        Request request = new Request(id, target);
        Bitmap previous = cached(clean, width, height);
        if (previous != null) target.setImageBitmap(previous);
        WORKER.execute(() -> {
            File file = new File(clean);
            if (clean.isEmpty() || !file.isFile()) { deliver(request, null); return; }
            String key = MessageImageDecodePolicy.cacheKey(clean, width, height, file.length(), file.lastModified());
            Bitmap hit = CACHE.get(key);
            if (hit != null) { deliver(request, hit); return; }
            synchronized (WAITING) {
                List<Request> pending = WAITING.get(key);
                if (pending != null) { pending.add(request); return; }
                pending = new ArrayList<>(); pending.add(request); WAITING.put(key, pending);
            }
            Bitmap bitmap = decode(clean, width, height);
            if (bitmap != null) {
                CACHE.put(key, bitmap);
                synchronized (RECENT) {
                    RECENT.put(logical(clean, width, height), new WeakReference<>(bitmap));
                    if (RECENT.size() > 512) RECENT.remove(RECENT.keySet().iterator().next());
                }
            }
            List<Request> pending;
            synchronized (WAITING) { pending = WAITING.remove(key); }
            if (pending != null) for (Request waiting : pending) deliver(waiting, bitmap);
        });
    }
    private static void deliver(Request request, Bitmap bitmap) {
        MAIN.post(() -> {
            ImageView view = request.view.get();
            if (view == null || !(view.getTag() instanceof Tag)) return;
            Tag tag = (Tag) view.getTag();
            if (tag.id != request.id) return;
            if (bitmap != null) {
                if (!(view.getDrawable() instanceof BitmapDrawable) || ((BitmapDrawable) view.getDrawable()).getBitmap() != bitmap) view.setImageBitmap(bitmap);
            } else if (tag.missing != null) tag.missing.onMissing();
        });
    }
    private static Bitmap decode(String path, int width, int height) {
        try {
            BitmapFactory.Options bounds = new BitmapFactory.Options(); bounds.inJustDecodeBounds = true;
            BitmapFactory.decodeFile(path, bounds);
            BitmapFactory.Options options = new BitmapFactory.Options();
            options.inSampleSize = MessageImageDecodePolicy.sampleSize(bounds.outWidth, bounds.outHeight, width, height);
            return BitmapFactory.decodeFile(path, options);
        } catch (OutOfMemoryError | RuntimeException error) { return null; }
    }
}
