// Captures frames only while the user-approved Android MediaProjection is active.
package com.kongshang.maichat.tools;

import android.app.Activity;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.graphics.Bitmap;
import android.graphics.PixelFormat;
import android.hardware.display.DisplayManager;
import android.hardware.display.VirtualDisplay;
import android.media.Image;
import android.media.ImageReader;
import android.media.projection.MediaProjection;
import android.media.projection.MediaProjectionManager;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.SystemClock;
import android.util.DisplayMetrics;
import android.view.WindowManager;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;

public final class MobileScreenCaptureService extends Service {
    private static final String CHANNEL = "maichat-screen-share";
    private static final int NOTIFICATION_ID = 34271;
    private static final String EXTRA_CONSENT = "screen_share_consent";
    private static final String EXTRA_RESULT = "screen_share_result";
    private static volatile MobileScreenCaptureService current;
    private static volatile boolean appForeground;

    private final Object frameLock = new Object();
    private HandlerThread captureThread;
    private ImageReader imageReader;
    private VirtualDisplay virtualDisplay;
    private MediaProjection projection;
    private File latestFrame;
    private volatile long frameElapsedMs;
    private volatile boolean capturing;

    public static void start(Activity activity, int resultCode, Intent consent) {
        Intent intent = new Intent(activity, MobileScreenCaptureService.class);
        intent.putExtra(EXTRA_RESULT, resultCode);
        intent.putExtra(EXTRA_CONSENT, consent);
        activity.startForegroundService(intent);
    }

    public static void stop(Context context) {
        context.stopService(new Intent(context, MobileScreenCaptureService.class));
    }

    public static boolean isCapturing() {
        MobileScreenCaptureService service = current;
        return service != null && service.capturing;
    }

    public static void setAppForeground(boolean foreground) {
        appForeground = foreground;
    }

    public static boolean awaitReady(long timeoutMs) throws InterruptedException {
        long deadline = SystemClock.elapsedRealtime() + timeoutMs;
        while (SystemClock.elapsedRealtime() < deadline) {
            MobileScreenCaptureService service = current;
            if (service != null && service.capturing) return true;
            Thread.sleep(100);
        }
        return false;
    }

    public static long copyLatestFrame(File output) throws IOException {
        MobileScreenCaptureService service = current;
        if (service == null || !service.capturing)
            throw new IllegalStateException("请先开启系统屏幕共享");
        synchronized (service.frameLock) {
            long age = SystemClock.elapsedRealtime() - service.frameElapsedMs;
            if (service.frameElapsedMs == 0 || age > 300_000 ||
                service.latestFrame == null || !service.latestFrame.isFile())
                throw new IllegalStateException("尚无最近的屏幕画面，请切换到目标 App 后再返回");
            File parent = output.getParentFile();
            if (parent == null || (!parent.isDirectory() && !parent.mkdirs()))
                throw new IOException("无法创建截图目录");
            Files.copy(service.latestFrame.toPath(), output.toPath(),
                StandardCopyOption.REPLACE_EXISTING);
            return age;
        }
    }

    @Override public IBinder onBind(Intent intent) { return null; }

    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent == null || !intent.hasExtra(EXTRA_CONSENT)) {
            stopSelf();
            return START_NOT_STICKY;
        }
        NotificationManager notifications = getSystemService(NotificationManager.class);
        notifications.createNotificationChannel(new NotificationChannel(
            CHANNEL, "MaiChat 屏幕共享", NotificationManager.IMPORTANCE_LOW));
        Notification notification = new Notification.Builder(this, CHANNEL)
            .setSmallIcon(android.R.drawable.ic_menu_camera)
            .setContentTitle("MaiChat 正在读取屏幕")
            .setContentText("返回 MaiChat 后可让 Agent 分析画面")
            .setOngoing(true).build();
        if (Build.VERSION.SDK_INT >= 29)
            startForeground(NOTIFICATION_ID, notification,
                ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION);
        else startForeground(NOTIFICATION_ID, notification);
        try {
            MediaProjectionManager manager = getSystemService(MediaProjectionManager.class);
            Intent consent = intent.getParcelableExtra(EXTRA_CONSENT);
            if (manager == null || consent == null) throw new IllegalStateException("没有系统授权");
            captureThread = new HandlerThread("MaiChatScreenCapture");
            captureThread.start();
            Handler handler = new Handler(captureThread.getLooper());
            projection = manager.getMediaProjection(intent.getIntExtra(EXTRA_RESULT, 0), consent);
            if (projection == null) throw new IllegalStateException("无法建立屏幕共享");
            projection.registerCallback(new MediaProjection.Callback() {
                @Override public void onStop() { stopSelf(); }
            }, handler);
            DisplayMetrics metrics = new DisplayMetrics();
            getSystemService(WindowManager.class).getDefaultDisplay().getRealMetrics(metrics);
            imageReader = ImageReader.newInstance(metrics.widthPixels, metrics.heightPixels,
                PixelFormat.RGBA_8888, 2);
            latestFrame = new File(getCacheDir(), "screen-share-latest.png");
            imageReader.setOnImageAvailableListener(this::writeFrame, handler);
            virtualDisplay = projection.createVirtualDisplay("MaiChat whole-device screen",
                metrics.widthPixels, metrics.heightPixels, metrics.densityDpi,
                DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR,
                imageReader.getSurface(), null, handler);
            capturing = true;
            current = this;
        } catch (RuntimeException error) {
            stopSelf();
        }
        return START_NOT_STICKY;
    }

    private void writeFrame(ImageReader reader) {
        Image image;
        try { image = reader.acquireLatestImage(); }
        catch (IllegalStateException closed) { return; }
        if (image == null) return;
        try {
            long now = SystemClock.elapsedRealtime();
            if (!capturing || appForeground || now - frameElapsedMs < 1_000) return;
            Image.Plane plane = image.getPlanes()[0];
            ByteBuffer buffer = plane.getBuffer();
            int width = image.getWidth();
            int height = image.getHeight();
            int paddedWidth = plane.getRowStride() / plane.getPixelStride();
            Bitmap padded = Bitmap.createBitmap(paddedWidth, height, Bitmap.Config.ARGB_8888);
            try {
                padded.copyPixelsFromBuffer(buffer);
                Bitmap frame = Bitmap.createBitmap(padded, 0, 0, width, height);
                try {
                    File temporary = new File(getCacheDir(), "screen-share-next.png");
                    try (FileOutputStream stream = new FileOutputStream(temporary)) {
                        if (!frame.compress(Bitmap.CompressFormat.PNG, 100, stream))
                            throw new IOException("PNG 编码失败");
                    }
                    synchronized (frameLock) {
                        // Returning to MaiChat can happen while PNG compression is in flight.
                        // Do not let that transition frame replace the target app's last frame.
                        if (!capturing || appForeground) {
                            temporary.delete();
                            return;
                        }
                        Files.move(temporary.toPath(), latestFrame.toPath(),
                            StandardCopyOption.REPLACE_EXISTING);
                        frameElapsedMs = now;
                    }
                } finally { frame.recycle(); }
            } finally { padded.recycle(); }
        } catch (RuntimeException | IOException ignored) {
            // A failed frame never replaces the last complete PNG.
        } finally {
            image.close();
        }
    }

    @Override public void onDestroy() {
        capturing = false;
        if (current == this) current = null;
        if (virtualDisplay != null) virtualDisplay.release();
        if (imageReader != null) imageReader.close();
        if (projection != null) projection.stop();
        if (captureThread != null) captureThread.quitSafely();
        synchronized (frameLock) {
            frameElapsedMs = 0;
            if (latestFrame != null) latestFrame.delete();
        }
        super.onDestroy();
    }
}
