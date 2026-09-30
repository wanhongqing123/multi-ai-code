package com.kongshang.maichat;

import android.content.Context;
import android.graphics.SurfaceTexture;
import android.os.Handler;
import android.os.Looper;
import android.view.Surface;
import android.view.TextureView;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.lang.ref.WeakReference;
import java.util.concurrent.ConcurrentHashMap;

/** An inline Graphics surface. Rendering and FFmpeg decoding stay off the UI thread. */
public final class MaiGraphicsTextureView extends TextureView
    implements TextureView.SurfaceTextureListener {

    public interface PresentationListener { void onPresented(boolean success); }

    private static final Handler MAIN = new Handler(Looper.getMainLooper());
    private static final ConcurrentHashMap<Long, WeakReference<MaiGraphicsTextureView>> VIEWS =
        new ConcurrentHashMap<>();
    private static final boolean LIBRARY_LOADED;
    private static volatile String effectsDirectory;

    static {
        boolean loaded;
        try {
            System.loadLibrary("maichat_agent");
            loaded = true;
        } catch (UnsatisfiedLinkError error) {
            loaded = false;
        }
        LIBRARY_LOADED = loaded;
    }

    private static native boolean nativeStart(String backend, String effectDirectory);
    private static native long nativeAttach(Surface surface, int width, int height);
    private static native boolean nativeShowImage(long viewId, String path, boolean fillView);
    private static native boolean nativeShowFrame(long viewId, byte[] rgba, int width,
                                                  int height, int stride, boolean fillView);
    private static native long nativeCreateVideo(long viewId, String path);
    private static native boolean nativePlayVideo(long videoHandle);
    private static native boolean nativePauseVideo(long videoHandle);
    private static native boolean nativeSeekVideo(long videoHandle, long positionMs);
    private static native void nativeDestroyVideo(long videoHandle);
    private static native void nativeResize(long viewId, int width, int height);
    private static native void nativeDetach(long viewId);

    private long viewId;
    private long videoHandle;
    private String imagePath;
    private boolean fillView;
    private PresentationListener listener;

    public MaiGraphicsTextureView(Context context) {
        super(context);
        setSurfaceTextureListener(this);
        setOpaque(true);
    }

    public void setImageFile(String path, boolean fill) {
        imagePath = path;
        fillView = fill;
        if (viewId != 0 && path != null && !nativeShowImage(viewId, path, fillView)) notifyFailure();
    }

    public boolean showRgbaFrame(byte[] rgba, int width, int height, int stride, boolean fill) {
        return viewId != 0 && nativeShowFrame(viewId, rgba, width, height, stride, fill);
    }

    public boolean playVideo(String path) {
        if (viewId == 0 || path == null) return false;
        stopVideo();
        videoHandle = nativeCreateVideo(viewId, path);
        return videoHandle != 0 && nativePlayVideo(videoHandle);
    }

    public boolean pauseVideo() {
        return videoHandle != 0 && nativePauseVideo(videoHandle);
    }

    public boolean seekVideo(long positionMs) {
        return videoHandle != 0 && nativeSeekVideo(videoHandle, positionMs);
    }

    public void stopVideo() {
        if (videoHandle == 0) return;
        nativeDestroyVideo(videoHandle);
        videoHandle = 0;
    }

    public void setPresentationListener(PresentationListener listener) {
        this.listener = listener;
    }

    private void notifyFailure() {
        if (listener != null) listener.onPresented(false);
    }

    private static String prepareEffects(Context context) {
        if (effectsDirectory != null) return effectsDirectory;
        synchronized (MaiGraphicsTextureView.class) {
            if (effectsDirectory != null) return effectsDirectory;
            File directory = new File(context.getFilesDir(), "MaiAgentGraphics");
            if (!directory.isDirectory() && !directory.mkdirs()) return null;
            String[] bundled;
            try {
                bundled = context.getAssets().list("MaiAgentGraphics");
            } catch (IOException error) {
                return null;
            }
            if (bundled == null || bundled.length == 0) return null;
            for (String name : bundled) {
                if (!name.endsWith(".effect")) continue;
                try (InputStream input = context.getAssets().open("MaiAgentGraphics/" + name);
                     FileOutputStream output = new FileOutputStream(new File(directory, name))) {
                    byte[] buffer = new byte[8192];
                    int size;
                    while ((size = input.read(buffer)) != -1) output.write(buffer, 0, size);
                } catch (IOException error) {
                    return null;
                }
            }
            if (!new File(directory, "default.effect").isFile()) return null;
            effectsDirectory = directory.getAbsolutePath();
            return effectsDirectory;
        }
    }

    @SuppressWarnings("unused")
    private static void onNativePresented(long id, boolean success) {
        MAIN.post(() -> {
            WeakReference<MaiGraphicsTextureView> reference = VIEWS.get(id);
            MaiGraphicsTextureView view = reference == null ? null : reference.get();
            if (view != null && view.viewId == id && view.listener != null)
                view.listener.onPresented(success);
        });
    }

    @Override public void onSurfaceTextureAvailable(SurfaceTexture texture, int width, int height) {
        if (!LIBRARY_LOADED) { notifyFailure(); return; }
        String effects = prepareEffects(getContext().getApplicationContext());
        String backend = getContext().getApplicationInfo().nativeLibraryDir
            + "/libmaiagent_obs_gles.so";
        if (effects == null || !nativeStart(backend, effects)) { notifyFailure(); return; }
        Surface surface = new Surface(texture);
        try {
            viewId = nativeAttach(surface, width, height);
        } finally {
            surface.release();
        }
        if (viewId == 0) { notifyFailure(); return; }
        VIEWS.put(viewId, new WeakReference<>(this));
        if (imagePath != null && !nativeShowImage(viewId, imagePath, fillView)) notifyFailure();
    }

    @Override public void onSurfaceTextureSizeChanged(SurfaceTexture texture, int width, int height) {
        if (viewId == 0) return;
        nativeResize(viewId, width, height);
    }

    @Override public boolean onSurfaceTextureDestroyed(SurfaceTexture texture) {
        stopVideo();
        if (viewId != 0) {
            VIEWS.remove(viewId);
            nativeDetach(viewId);
            viewId = 0;
        }
        return true;
    }

    @Override public void onSurfaceTextureUpdated(SurfaceTexture texture) {}
}
