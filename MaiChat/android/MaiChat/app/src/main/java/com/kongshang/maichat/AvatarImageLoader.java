package com.kongshang.maichat;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.util.LruCache;
import android.widget.ImageView;

import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public final class AvatarImageLoader {
    private static final LruCache<String, Bitmap> CACHE = new LruCache<>(32);
    private static final ExecutorService EXECUTOR = Executors.newFixedThreadPool(2);

    private AvatarImageLoader() {
    }

    public static void load(String rawUrl, ImageView target) {
        String value = rawUrl == null ? "" : rawUrl.trim();
        if (!(value.startsWith("https://") || value.startsWith("http://"))) return;
        Bitmap cached = CACHE.get(value);
        if (cached != null) {
            target.setImageBitmap(cached);
            return;
        }
        target.setTag(value);
        EXECUTOR.execute(() -> {
            Bitmap bitmap = download(value);
            if (bitmap == null) return;
            CACHE.put(value, bitmap);
            target.post(() -> {
                if (value.equals(target.getTag())) target.setImageBitmap(bitmap);
            });
        });
    }

    private static Bitmap download(String value) {
        HttpURLConnection connection = null;
        try {
            connection = (HttpURLConnection) new URL(value).openConnection();
            connection.setConnectTimeout(5_000);
            connection.setReadTimeout(8_000);
            connection.setInstanceFollowRedirects(true);
            try (InputStream input = connection.getInputStream()) {
                return BitmapFactory.decodeStream(input);
            }
        } catch (Exception error) {
            return null;
        } finally {
            if (connection != null) connection.disconnect();
        }
    }
}
