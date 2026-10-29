// 文件作用：用 App 已链接的 libcurl WebSocket 传输腾讯实时语音的二进制音频与 JSON 响应。
// Java 层选择 Android 当前信任证书和代理；原生 I/O 线程独占 curl 句柄。
package com.kongshang.maichat;

import java.io.IOException;
import java.net.URL;
import java.nio.charset.StandardCharsets;

final class CurlRealtimeSpeechWebSocket {
    interface Listener {
        void message(String text);
        void failed(int code);
        void closed();
    }

    private static final int RESULT_CLOSED = 0;
    private static final int RESULT_CANCELLED = -1;
    private static final int RESULT_SETUP_FAILED = -2;

    private final String url;
    private final String token;
    private final byte[] caPem;
    private final MaiChatAuthTransport.ProxyRoute proxy;
    private final Listener listener;
    private long handle;
    private boolean cancelled;

    CurlRealtimeSpeechWebSocket(String url, String token, Listener listener) throws Exception {
        this(url, token, SystemCertificateBundle.currentPem(), selectProxy(url), listener);
    }

    // 本地 WSS 测试使用临时 CA；正式路径始终读取 Android 当前系统信任证书。
    CurlRealtimeSpeechWebSocket(String url, byte[] caPem, MaiChatAuthTransport.ProxyRoute proxy,
                                Listener listener) throws IOException {
        this(url, "", caPem, proxy, listener);
    }

    // 只允许传入固定头的值，不暴露任意 HTTP 头注入；本地 WSS 合同测试可使用空值。
    CurlRealtimeSpeechWebSocket(String url, String token, byte[] caPem,
                                MaiChatAuthTransport.ProxyRoute proxy, Listener listener) throws IOException {
        if (url == null || (!url.startsWith("wss://") && !url.startsWith("ws://"))
                || url.length() > 4096 || caPem == null || proxy == null || listener == null
                || token == null || !token.matches("[!-~]{0,8192}"))
            throw new IOException("实时语音连接参数无效");
        this.url = url;
        this.token = token;
        this.caPem = caPem.clone();
        this.proxy = proxy;
        this.listener = listener;
        try { System.loadLibrary("maichat_agent"); }
        catch (LinkageError error) { throw new IOException("实时语音网络组件不可用", error); }
        handle = nativeCreate();
        if (handle == 0) throw new IOException("实时语音网络组件初始化失败");
        try {
            new Thread(this::run, "MaiChat-asr-wss").start();
        } catch (RuntimeException | Error error) {
            nativeDestroy(handle);
            handle = 0;
            throw error;
        }
    }

    private static MaiChatAuthTransport.ProxyRoute selectProxy(String url) throws IOException {
        if (url == null || !url.startsWith("wss://"))
            throw new IOException("实时语音服务地址无效");
        return MaiChatAuthTransport.selectedProxy(new URL("https" + url.substring(3)));
    }

    synchronized boolean sendBinary(byte[] packet) {
        return !cancelled && handle != 0 && nativeQueue(handle, packet, false);
    }

    synchronized boolean sendText(String text) {
        return !cancelled && handle != 0
            && nativeQueue(handle, text.getBytes(StandardCharsets.UTF_8), true);
    }

    synchronized void cancel() {
        if (cancelled) return;
        cancelled = true;
        if (handle != 0) nativeCancel(handle);
    }

    private void run() {
        long target;
        synchronized (this) { target = handle; }
        int result = RESULT_SETUP_FAILED;
        try {
            result = nativeRun(target, url, token, caPem, proxy.type, proxy.host, proxy.port);
        } catch (Throwable ignored) { }
        boolean notify;
        synchronized (this) {
            notify = !cancelled;
            handle = 0;
            nativeDestroy(target);
        }
        if (notify) {
            if (result == RESULT_CLOSED) listener.closed();
            else if (result != RESULT_CANCELLED) listener.failed(result);
        }
    }

    // 由原生 I/O 线程调用；订阅者负责把解析工作转到自己的串行 worker。
    private void onMessage(byte[] utf8) {
        listener.message(new String(utf8, StandardCharsets.UTF_8));
    }

    private static native long nativeCreate();
    private static native boolean nativeQueue(long handle, byte[] packet, boolean text);
    private static native void nativeCancel(long handle);
    private static native void nativeDestroy(long handle);
    private native int nativeRun(long handle, String url, String token, byte[] caPem, int proxyType,
                                 String proxyHost, int proxyPort);
}
