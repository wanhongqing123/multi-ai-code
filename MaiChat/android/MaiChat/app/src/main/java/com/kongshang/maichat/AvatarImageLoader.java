package com.kongshang.maichat;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.util.LruCache;
import android.widget.ImageView;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.lang.ref.WeakReference;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public final class AvatarImageLoader {
    private static final int MAX_CACHE_BYTES = 8 * 1024 * 1024;
    private static final int MAX_DOWNLOAD_BYTES = 4 * 1024 * 1024;
    private static final int TARGET_PIXEL_SIZE = 160;
    private static final LruCache<String, Bitmap> CACHE = new LruCache<String, Bitmap>(
        MAX_CACHE_BYTES
    ) {
        @Override
        protected int sizeOf(String key, Bitmap bitmap) {
            return bitmap.getAllocationByteCount();
        }
    };
    private static final ExecutorService EXECUTOR = Executors.newFixedThreadPool(2);
    private static final Map<String, List<WeakReference<ImageView>>> WAITING_TARGETS =
        new HashMap<>();

    private AvatarImageLoader() {
    }

    public static void load(String rawUrl, ImageView target) {
        String value = rawUrl == null ? "" : rawUrl.trim();
        if (!(value.startsWith("https://") || value.startsWith("http://"))) return;
        Bitmap cached;
        synchronized (CACHE) {
            cached = CACHE.get(value);
        }
        if (cached != null) {
            target.setImageBitmap(cached);
            return;
        }
        target.setTag(value);
        synchronized (WAITING_TARGETS) {
            List<WeakReference<ImageView>> targets = WAITING_TARGETS.get(value);
            if (targets != null) {
                targets.add(new WeakReference<>(target));
                return;
            }
            targets = new ArrayList<>();
            targets.add(new WeakReference<>(target));
            WAITING_TARGETS.put(value, targets);
        }
        EXECUTOR.execute(() -> finish(value, download(value)));
    }

    private static void finish(String value, Bitmap bitmap) {
        if (bitmap != null) {
            synchronized (CACHE) {
                CACHE.put(value, bitmap);
            }
        }
        List<WeakReference<ImageView>> targets;
        synchronized (WAITING_TARGETS) {
            targets = WAITING_TARGETS.remove(value);
        }
        if (bitmap == null || targets == null) return;
        for (WeakReference<ImageView> reference : targets) {
            ImageView target = reference.get();
            if (target == null) continue;
            target.post(() -> {
                if (value.equals(target.getTag())) target.setImageBitmap(bitmap);
            });
        }
    }

    private static Bitmap download(String value) {
        HttpURLConnection connection = null;
        try {
            connection = (HttpURLConnection) new URL(value).openConnection();
            connection.setConnectTimeout(5_000);
            connection.setReadTimeout(8_000);
            connection.setInstanceFollowRedirects(true);
            try (InputStream input = connection.getInputStream()) {
                byte[] data = readBounded(input);
                BitmapFactory.Options bounds = new BitmapFactory.Options();
                bounds.inJustDecodeBounds = true;
                BitmapFactory.decodeByteArray(data, 0, data.length, bounds);
                BitmapFactory.Options decode = new BitmapFactory.Options();
                decode.inSampleSize = sampleSize(bounds.outWidth, bounds.outHeight);
                decode.inPreferredConfig = Bitmap.Config.RGB_565;
                return BitmapFactory.decodeByteArray(data, 0, data.length, decode);
            }
        } catch (Exception error) {
            return null;
        } finally {
            if (connection != null) connection.disconnect();
        }
    }

    private static byte[] readBounded(InputStream input) throws Exception {
        ByteArrayOutputStream output = new ByteArrayOutputStream(32 * 1024);
        byte[] buffer = new byte[8 * 1024];
        int total = 0;
        int count;
        while ((count = input.read(buffer)) >= 0) {
            total += count;
            if (total > MAX_DOWNLOAD_BYTES) throw new IllegalStateException("avatar too large");
            output.write(buffer, 0, count);
        }
        return output.toByteArray();
    }

    private static int sampleSize(int width, int height) {
        int size = 1;
        while (width / size > TARGET_PIXEL_SIZE * 2 || height / size > TARGET_PIXEL_SIZE * 2) {
            size *= 2;
        }
        return Math.max(1, size);
    }
}
