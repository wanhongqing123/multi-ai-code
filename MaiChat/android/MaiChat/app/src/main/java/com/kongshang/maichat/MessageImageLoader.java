package com.kongshang.maichat;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.util.LruCache;
import android.widget.ImageView;

import java.io.File;
import java.lang.ref.WeakReference;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * 消息里的图片/视频封面加载。与 AvatarImageLoader 同一套做法，区别是读本地文件。
 *
 * 为什么必须有这一层：原先是在主线程直接 BitmapFactory.decodeFile 解原图。
 * 手机拍的 12MP 照片解出来约 48MB，单张耗时几十毫秒，而且界面每重建一次就重解一遍——
 * 一个有十张图的会话，切进去就是肉眼可见的停顿。
 *
 * 三端（Qt / Android / iOS）约定一致：按显示尺寸降采样、缓存键含目标尺寸、
 * 按字节限容 32MB、后台解码主线程贴图、同键并发只解一次。
 */
public final class MessageImageLoader {
    private static final int MAX_CACHE_BYTES = 32 * 1024 * 1024;

    private static final LruCache<String, Bitmap> CACHE = new LruCache<String, Bitmap>(MAX_CACHE_BYTES) {
        @Override
        protected int sizeOf(String key, Bitmap bitmap) {
            return bitmap.getAllocationByteCount();
        }
    };
    private static final ExecutorService EXECUTOR = Executors.newFixedThreadPool(2);
    // 同一个键正在解码时，后来的请求排队等结果，不重复解。
    private static final Map<String, List<WeakReference<ImageView>>> WAITING = new HashMap<>();

    /** 文件不存在或解不出来时的处理，交给调用方决定显示什么。 */
    public interface MissingHandler {
        void onMissing();
    }

    private MessageImageLoader() {
    }

    /** 已缓存则同步返回，否则 null——调用方据此决定先摆占位还是直接贴图。 */
    public static Bitmap cached(String path, int targetWidth, int targetHeight) {
        final File file = new File(path == null ? "" : path.trim());
        final String key = MessageImageDecodePolicy.cacheKey(
            path, targetWidth, targetHeight, file.length(), file.lastModified());
        synchronized (CACHE) {
            return CACHE.get(key);
        }
    }

    public static void load(String path, int targetWidth, int targetHeight, ImageView target,
                            MissingHandler onMissing) {
        final String cleanPath = path == null ? "" : path.trim();
        final File file = new File(cleanPath);
        if (cleanPath.isEmpty() || !file.isFile()) {
            if (onMissing != null) onMissing.onMissing();
            return;
        }
        // 指纹进键：文件内容变了（下载完成覆盖、同一条消息重新接收）键就变，
        // 不会一直贴着旧图，也就不需要另写失效逻辑。
        final String key = MessageImageDecodePolicy.cacheKey(
            cleanPath, targetWidth, targetHeight, file.length(), file.lastModified());
        Bitmap hit;
        synchronized (CACHE) {
            hit = CACHE.get(key);
        }
        if (hit != null) {
            target.setImageBitmap(hit);
            return;
        }
        // tag 记住这个 view 当前等的是哪个键：列表复用后 view 可能已经换了内容，
        // 迟到的解码结果不能贴到已经变成别的消息的那个 view 上。
        target.setTag(key);
        synchronized (WAITING) {
            List<WeakReference<ImageView>> queued = WAITING.get(key);
            if (queued != null) {
                queued.add(new WeakReference<>(target));
                return;
            }
            List<WeakReference<ImageView>> created = new ArrayList<>();
            created.add(new WeakReference<>(target));
            WAITING.put(key, created);
        }
        EXECUTOR.execute(() -> finish(key, decode(cleanPath, targetWidth, targetHeight), onMissing));
    }

    private static void finish(String key, Bitmap bitmap, MissingHandler onMissing) {
        if (bitmap != null) {
            synchronized (CACHE) {
                CACHE.put(key, bitmap);
            }
        }
        List<WeakReference<ImageView>> targets;
        synchronized (WAITING) {
            targets = WAITING.remove(key);
        }
        if (targets == null) return;
        for (WeakReference<ImageView> reference : targets) {
            final ImageView target = reference.get();
            if (target == null) continue;
            target.post(() -> {
                if (!key.equals(target.getTag())) return;
                if (bitmap != null) {
                    target.setImageBitmap(bitmap);
                } else if (onMissing != null) {
                    onMissing.onMissing();
                }
            });
        }
    }

    private static Bitmap decode(String path, int targetWidth, int targetHeight) {
        try {
            BitmapFactory.Options bounds = new BitmapFactory.Options();
            bounds.inJustDecodeBounds = true;
            BitmapFactory.decodeFile(path, bounds);
            BitmapFactory.Options options = new BitmapFactory.Options();
            options.inSampleSize = MessageImageDecodePolicy.sampleSize(
                bounds.outWidth, bounds.outHeight, targetWidth, targetHeight);
            return BitmapFactory.decodeFile(path, options);
        } catch (OutOfMemoryError | RuntimeException error) {
            return null;
        }
    }
}
