// Android UI adapter for the shared C++ AI video circle and marked-MP4 clients.
package com.kongshang.maichat;

import android.content.Context;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import java.io.File;
import java.io.IOException;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.UUID;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.function.Consumer;
import org.json.JSONObject;

final class MaiVideoCircle {
    static final int SIGN_UPLOAD = 1, PUBLISH_DRAFT = 2, FEED = 3, MINE = 4,
        PLAY_LINK = 5, WITHDRAW = 6, REPORT = 7, BLOCK = 8, UNBLOCK = 9,
        BLOCKS_LIST = 10;
    static final int OK = 0;
    private static final int MAX_VIDEO_BYTES = 200_000_000;
    private static final ExecutorService WORKER = Executors.newSingleThreadExecutor(
        runnable -> new Thread(runnable, "maichat-video-circle"));
    private static final Handler MAIN = new Handler(Looper.getMainLooper());
    private static volatile byte[] localFixturePem;

    static final class Result {
        final int status;
        final long httpStatus;
        final int curlCode;
        final String body;
        Result(int status, long httpStatus, int curlCode, byte[] body) {
            this.status = status;
            this.httpStatus = httpStatus;
            this.curlCode = curlCode;
            this.body = body == null ? "" : new String(body, StandardCharsets.UTF_8);
        }
        boolean ok() { return status == OK; }
        JSONObject json() {
            try { return new JSONObject(body); }
            catch (Exception ignored) { return new JSONObject(); }
        }
    }

    static final class MarkResult {
        final int status;
        final long bytes;
        final long durationMs;
        final boolean visibleVerified;
        final boolean metadataVerified;
        final String sha256;
        final String detail;
        MarkResult(int status, long bytes, long durationMs, boolean visibleVerified,
                   boolean metadataVerified, String sha256, String detail) {
            this.status = status;
            this.bytes = bytes;
            this.durationMs = durationMs;
            this.visibleVerified = visibleVerified;
            this.metadataVerified = metadataVerified;
            this.sha256 = sha256;
            this.detail = detail;
        }
        boolean ok() {
            return status == OK && visibleVerified && metadataVerified &&
                bytes > 0 && bytes <= MAX_VIDEO_BYTES && durationMs > 0 && durationMs <= 60_000;
        }
    }

    static final class PublishResult {
        final Result result;
        final String message;
        PublishResult(Result result, String message) {
            this.result = result;
            this.message = message;
        }
        boolean ok() { return result != null && result.ok(); }
    }

    private static final class NativeLibrary {
        static { System.loadLibrary("maichat_agent"); }
        static void ensureLoaded() { }
    }

    private final Context context;
    private final MaiChatAuthClient auth;
    private final byte[] testCaPem;
    private final String testAuthBase;
    private final String testMediaBase;
    private final String testUploadHostSuffix;
    private final Object operationLock = new Object();
    private long currentCancel;
    private long currentMarkCancel;
    private volatile long operationVersion;

    MaiVideoCircle(Context context) {
        this(context, null, null,
            BuildConfig.DEBUG && BuildConfig.USE_LOCAL_UI_AUTH ? localFixturePem : null,
            BuildConfig.DEBUG && BuildConfig.USE_LOCAL_UI_AUTH &&
                localFixturePem != null ? "localhost" : null);
    }

    static void setLocalFixturePemForTest(byte[] pem) {
        if (BuildConfig.DEBUG && BuildConfig.USE_LOCAL_UI_AUTH) localFixturePem = pem;
    }

    // Local TLS fixture injection is for emulator tests only. Production never reads a test CA.
    MaiVideoCircle(Context context, String authBase, String mediaBase, byte[] caPem,
                   String uploadHostSuffix) {
        this.context = context.getApplicationContext();
        this.auth = MaiChatAuthClient.shared(context);
        this.testAuthBase = authBase;
        this.testMediaBase = mediaBase;
        this.testCaPem = caPem;
        this.testUploadHostSuffix = uploadHostSuffix;
    }

    static Result inspectCard(JSONObject completedPart) {
        try {
            NativeLibrary.ensureLoaded();
            Result result = nativeInspect(completedPart == null ? "" : completedPart.toString());
            return result == null ? new Result(2, 0, 0, null) : result;
        } catch (LinkageError failure) {
            return new Result(1, 0, 0, null);
        }
    }

    void cancel() {
        synchronized (operationLock) {
            operationVersion++;
            if (currentCancel != 0) nativeRequestCancel(currentCancel);
            if (currentMarkCancel != 0) nativeMarkCancel(currentMarkCancel);
        }
    }

    private long nextOperation() {
        synchronized (operationLock) {
            operationVersion++;
            if (currentCancel != 0) nativeRequestCancel(currentCancel);
            if (currentMarkCancel != 0) nativeMarkCancel(currentMarkCancel);
            return operationVersion;
        }
    }

    void request(int action, JSONObject payload, Consumer<Result> completion) {
        long operation = nextOperation();
        String account = auth.accountId();
        WORKER.execute(() -> {
            Result result = requestNow(action, payload, operation);
            MAIN.post(() -> {
                if (operationVersion != operation) return;
                completion.accept(account.equals(auth.accountId()) ? result :
                    new Result(6, 0, 0, null));
            });
        });
    }

    private Result requestNow(int action, JSONObject payload, long operation) {
        if (operationVersion != operation) return new Result(6, 0, 0, null);
        long handle = 0;
        try {
            NativeLibrary.ensureLoaded();
            MaiChatAuthClient.VideoCircleAccess access = auth.videoCircleAccess();
            if (access == null) return new Result(1, 401, 0, null);
            byte[] ca = testCaPem == null ? SystemCertificateBundle.currentPem() : testCaPem;
            handle = nativeCreateCancel();
            if (handle == 0) return new Result(2, 0, 0, null);
            synchronized (operationLock) {
                if (operationVersion != operation) return new Result(6, 0, 0, null);
                currentCancel = handle;
            }
            Result result = nativeRequest(testAuthBase == null ? access.authBaseUrl : testAuthBase,
                testMediaBase == null ? access.mediaBaseUrl : testMediaBase,
                access.accountToken, ca, action,
                payload == null ? "{}" : payload.toString(), handle);
            return result == null ? new Result(2, 0, 0, null) : result;
        } catch (Exception | LinkageError failure) {
            return new Result(2, 0, 0, null);
        } finally {
            synchronized (operationLock) {
                if (currentCancel == handle) currentCancel = 0;
                if (handle != 0) nativeDestroyCancel(handle);
            }
        }
    }

    void publish(JSONObject completedPart, File original, String title,
                 Consumer<PublishResult> completion) {
        long operation = nextOperation();
        String account = auth.accountId();
        WORKER.execute(() -> {
            PublishResult result = publishNow(completedPart, original, title, operation, account);
            MAIN.post(() -> {
                if (operationVersion != operation) return;
                completion.accept(account.equals(auth.accountId()) ? result :
                    new PublishResult(null, "账号已切换，发布已取消"));
            });
        });
    }

    private PublishResult publishNow(JSONObject completedPart, File original, String title,
                                     long operation, String account) {
        File marked = null;
        long markHandle = 0;
        try {
            if (operationVersion != operation || !account.equals(auth.accountId()) ||
                completedPart == null || original == null || !original.isFile())
                return new PublishResult(null, "原视频不存在");
            if (!inspectCard(completedPart).ok())
                return new PublishResult(null, "仅已完成且有来源记录的 AI 视频可发布");
            File cache = new File(context.getCacheDir(), "video-circle");
            if (!cache.isDirectory() && !cache.mkdirs())
                throw new IOException("无法准备视频缓存");
            marked = new File(cache, UUID.randomUUID() + ".mp4");
            markHandle = nativeCreateMarkCancel();
            if (markHandle == 0) return new PublishResult(null, "视频标识组件不可用");
            synchronized (operationLock) {
                if (operationVersion != operation) return new PublishResult(null, "已取消发布");
                currentMarkCancel = markHandle;
            }
            MarkResult prepared = nativeMark(original.getAbsolutePath(),
                marked.getAbsolutePath(), markHandle);
            synchronized (operationLock) {
                currentMarkCancel = 0;
                nativeDestroyMarkCancel(markHandle);
            }
            markHandle = 0;
            if (operationVersion != operation || !account.equals(auth.accountId()))
                return new PublishResult(null, "已取消发布");
            if (prepared == null || !prepared.ok()) {
                Log.w("MaiVideoCircle", "Mark failed: status=" +
                    (prepared == null ? -1 : prepared.status) + " detail=" +
                    (prepared == null ? "null" : prepared.detail));
                return new PublishResult(null, "视频标识或格式校验失败");
            }
            JSONObject sign = new JSONObject().put("filename", "video.mp4")
                .put("size_bytes", prepared.bytes);
            Result signed = requestNow(SIGN_UPLOAD, sign, operation);
            if (!signed.ok()) return new PublishResult(signed, failureText(signed));
            if (operationVersion != operation || !account.equals(auth.accountId()))
                return new PublishResult(null, "已取消发布");
            Result uploaded = uploadNow(signed.body, marked, prepared.bytes, operation);
            if (!uploaded.ok()) return new PublishResult(uploaded, failureText(uploaded));
            if (operationVersion != operation || !account.equals(auth.accountId()))
                return new PublishResult(null, "已取消发布");
            JSONObject submission = new JSONObject()
                .put("completed_card", completedPart)
                .put("title", title)
                .put("object_key", signed.json().optString("object_key"))
                .put("size_bytes", prepared.bytes)
                .put("duration_ms", prepared.durationMs);
            Result published = requestNow(PUBLISH_DRAFT, submission, operation);
            return new PublishResult(published,
                published.ok() ? "已提交审核" : failureText(published));
        } catch (Exception | LinkageError failure) {
            return new PublishResult(null, "发布失败，请稍后重试");
        } finally {
            synchronized (operationLock) {
                if (currentMarkCancel == markHandle) currentMarkCancel = 0;
                if (markHandle != 0) nativeDestroyMarkCancel(markHandle);
            }
            if (marked != null) marked.delete();
        }
    }

    private Result uploadNow(String signedJson, File marked, long bytes, long operation) {
        long handle = 0;
        try {
            if (operationVersion != operation) return new Result(6, 0, 0, null);
            MaiChatAuthClient.VideoCircleAccess access = auth.videoCircleAccess();
            if (access == null) return new Result(1, 401, 0, null);
            byte[] ca = testCaPem == null ? SystemCertificateBundle.currentPem() : testCaPem;
            handle = nativeCreateCancel();
            if (handle == 0) return new Result(2, 0, 0, null);
            synchronized (operationLock) {
                if (operationVersion != operation) return new Result(6, 0, 0, null);
                currentCancel = handle;
            }
            Result result = nativeUpload(access.accountToken, ca, signedJson,
                marked.getAbsolutePath(), bytes, testUploadHostSuffix, handle);
            return result == null ? new Result(2, 0, 0, null) : result;
        } catch (Exception | LinkageError failure) {
            return new Result(2, 0, 0, null);
        } finally {
            synchronized (operationLock) {
                if (currentCancel == handle) currentCancel = 0;
                if (handle != 0) nativeDestroyCancel(handle);
            }
        }
    }

    static String failureText(Result result) {
        if (result == null) return "操作失败，请稍后重试";
        if (result.httpStatus == 401) return "请登录 MaiChat 后重试";
        if (result.httpStatus == 404) return "内容已不可用";
        if (result.httpStatus == 429) return "待审核视频已达上限，请稍后再试";
        if (result.status == 6) return "已取消";
        return "网络或服务暂不可用，请稍后重试";
    }

    byte[] playbackTrustPem() throws Exception {
        return testCaPem == null ? SystemCertificateBundle.currentPem() : testCaPem;
    }

    private static String proxyForUrl(String url) throws IOException {
        MaiChatAuthTransport.ProxyRoute route =
            MaiChatAuthTransport.selectedProxy(new URL(url));
        if (route.type == MaiChatAuthTransport.ProxyRoute.DIRECT) return "";
        String host = route.host.indexOf(':') >= 0 ? "[" + route.host + "]" : route.host;
        return (route.type == MaiChatAuthTransport.ProxyRoute.HTTP ? "http://" : "socks5h://")
            + host + ":" + route.port;
    }

    private static native Result nativeInspect(String cardJson);
    private static native long nativeCreateCancel();
    private static native void nativeRequestCancel(long handle);
    private static native void nativeDestroyCancel(long handle);
    private static native Result nativeRequest(String authBase, String mediaBase, String token,
        byte[] caPem, int action, String payload, long cancel);
    private static native Result nativeUpload(String token, byte[] caPem, String signedJson,
        String sourcePath, long bytes, String hostSuffix, long cancel);
    private static native long nativeCreateMarkCancel();
    private static native void nativeMarkCancel(long handle);
    private static native void nativeDestroyMarkCancel(long handle);
    private static native MarkResult nativeMark(String sourcePath, String targetPath, long cancel);
}
