package com.kongshang.maichat;

import java.io.IOException;
import java.util.UUID;

/** Android lifetime adapter for the shared in-memory cloud Range reader. */
final class MaiCloudVod {
    private static final class NativeLibrary {
        static { System.loadLibrary("maichat_agent"); }
        static void ensureLoaded() { }
    }

    static final class Opening {
        private final long cancelHandle;
        private boolean canceled;
        private boolean released;

        Opening() throws IOException {
            NativeLibrary.ensureLoaded();
            cancelHandle = nativeCreateCancel();
            if (cancelHandle == 0) throw new IOException("无法启动云盘点播");
        }

        synchronized long handle() { return released ? 0 : cancelHandle; }
        synchronized boolean isCanceled() { return canceled; }

        synchronized void cancel() {
            if (released || canceled) return;
            canceled = true;
            nativeRequestCancel(cancelHandle);
        }

        synchronized void release() {
            if (released) return;
            released = true;
            nativeDestroyCancel(cancelHandle);
        }
    }

    static final class Session {
        private long sourceHandle;
        private final Opening opening;
        final String displayPath;

        private Session(long sourceHandle, Opening opening, String extension) {
            this.sourceHandle = sourceHandle;
            this.opening = opening;
            displayPath = "maicloud-vod-" + UUID.randomUUID() + "." + extension;
        }

        synchronized long handle() { return sourceHandle; }

        synchronized void cancel() {
            opening.cancel();
            if (sourceHandle != 0) nativeCancelSource(sourceHandle);
        }

        synchronized void close() {
            if (sourceHandle != 0) {
                nativeCloseSource(sourceHandle);
                sourceHandle = 0;
            }
            opening.release();
        }
    }

    static Session openBlocking(String base, String token, byte[] pem,
                                String suffix, String mediaID,
                                String extension, Opening opening) throws IOException {
        if (opening == null || opening.handle() == 0 || opening.isCanceled())
            throw new IOException("已取消云盘点播");
        long source = nativeOpen(base, token, pem, suffix, mediaID, opening.handle());
        if (source == 0) throw new IOException(opening.isCanceled()
            ? "已取消云盘点播" : "云盘视频点播失败，请重试");
        if (opening.isCanceled()) {
            nativeCloseSource(source);
            throw new IOException("已取消云盘点播");
        }
        return new Session(source, opening, extension);
    }

    private static native long nativeCreateCancel();
    private static native void nativeRequestCancel(long handle);
    private static native void nativeDestroyCancel(long handle);
    private static native long nativeOpen(String base, String token, byte[] pem,
        String suffix, String mediaID, long cancel);
    private static native void nativeCancelSource(long source);
    private static native void nativeCloseSource(long source);
}
