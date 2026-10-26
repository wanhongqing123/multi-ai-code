// Android adapters for the shared MaiChat libcurl attachment downloader.
package com.kongshang.maichat;

import java.io.File;
import java.io.IOException;
import java.net.URL;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

final class MaiChatAttachmentDownload {
    static final long MAX_IMAGE_BYTES = 32L * 1024 * 1024;
    static final long MAX_VOICE_BYTES = 64L * 1024 * 1024;
    static final long MAX_FILE_BYTES = 512L * 1024 * 1024;
    static final long MAX_VIDEO_BYTES = 1024L * 1024 * 1024;
    static final long MAX_VIDEO_COVER_BYTES = 16L * 1024 * 1024;

    static final int OK = 0;
    static final int INVALID_ARGUMENT = 1;
    static final int NETWORK_ERROR = 2;
    static final int HTTP_ERROR = 3;
    static final int TOO_LARGE = 4;
    static final int WRITE_ERROR = 5;
    static final int CANCELED = 6;

    interface Completion { void finished(Result result); }
    interface Transport {
        void download(String url, File target, long maximumBytes, Scope scope, Completion completion);
    }
    interface TrustSource { byte[] pem() throws Exception; }

    static final class Result {
        final int status;
        final long httpStatus;
        final int curlCode;
        final long bytes;

        Result(int status, long httpStatus, int curlCode, long bytes) {
            this.status = status;
            this.httpStatus = httpStatus;
            this.curlCode = curlCode;
            this.bytes = bytes;
        }

        boolean ok() { return status == OK; }
    }

    // One cancellation flag belongs to one account connection. Every transfer borrows it until
    // its native call returns; logging out cancels all queued and active downloads immediately.
    static final class Scope {
        private boolean canceled;
        private long nativeHandle;
        private int active;

        synchronized boolean isCanceled() { return canceled; }

        synchronized long acquire() {
            if (canceled) return 0;
            if (nativeHandle == 0) {
                try {
                    NativeLibrary.ensureLoaded();
                    nativeHandle = nativeCreateCancel();
                } catch (LinkageError failure) {
                    return 0;
                }
            }
            if (nativeHandle != 0) active++;
            return nativeHandle;
        }

        synchronized void release() {
            if (active <= 0) throw new IllegalStateException("unbalanced attachment download");
            active--;
            if (canceled && active == 0 && nativeHandle != 0) {
                nativeDestroyCancel(nativeHandle);
                nativeHandle = 0;
            }
        }

        synchronized void cancel() {
            if (canceled) return;
            canceled = true;
            if (nativeHandle != 0) {
                nativeRequestCancel(nativeHandle);
                if (active == 0) {
                    nativeDestroyCancel(nativeHandle);
                    nativeHandle = 0;
                }
            }
        }
    }

    private static final class NativeLibrary {
        static { System.loadLibrary("maichat_agent"); }
        static void ensureLoaded() { }
    }

    private static final ExecutorService WORKERS = Executors.newFixedThreadPool(3);

    static final class NativeTransport implements Transport {
        private final TrustSource trustSource;

        NativeTransport() { this(SystemCertificateBundle::currentPem); }
        NativeTransport(TrustSource trustSource) { this.trustSource = trustSource; }

        @Override public void download(String url, File target, long maximumBytes, Scope scope,
                                       Completion completion) {
            WORKERS.execute(() -> {
                Result result;
                long handle = scope.acquire();
                if (handle == 0) {
                    result = new Result(scope.isCanceled() ? CANCELED : NETWORK_ERROR, 0, 0, 0);
                } else {
                    try {
                        URL parsed = new URL(url);
                        if (!"https".equals(parsed.getProtocol()) || parsed.getHost().isEmpty()
                            || parsed.getUserInfo() != null || parsed.getRef() != null
                            || url.length() > 16384 || target == null || maximumBytes <= 0)
                            throw new IOException("invalid attachment download request");
                        byte[] caPem = trustSource.pem();
                        result = nativeDownload(url, target.getAbsolutePath(), maximumBytes,
                            caPem, handle);
                        if (result == null) result = new Result(NETWORK_ERROR, 0, 0, 0);
                    } catch (Exception | LinkageError failure) {
                        result = new Result(NETWORK_ERROR, 0, 0, 0);
                    } finally {
                        scope.release();
                    }
                }
                completion.finished(result);
            });
        }

    }

    // Invoked by the JNI adapter for the original URL and each HTTPS redirect.
    // Android PAC/system proxy rules can select a different route for each host.
    private static String proxyForUrl(String url) throws IOException {
        MaiChatAuthTransport.ProxyRoute route = MaiChatAuthTransport.selectedProxy(new URL(url));
        if (route.type == MaiChatAuthTransport.ProxyRoute.DIRECT) return "";
        String host = route.host.indexOf(':') >= 0 ? "[" + route.host + "]" : route.host;
        return (route.type == MaiChatAuthTransport.ProxyRoute.HTTP ? "http://" : "socks5h://")
            + host + ":" + route.port;
    }

    private MaiChatAttachmentDownload() { }

    private static native long nativeCreateCancel();
    private static native void nativeRequestCancel(long handle);
    private static native void nativeDestroyCancel(long handle);
    private static native Result nativeDownload(String url, String targetPath, long maximumBytes,
        byte[] caPem, long cancelHandle);
}
