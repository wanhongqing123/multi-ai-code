// 文件作用：用 App 内的原生 libcurl 执行账号 HTTP 请求，保留 Android 当前代理和证书信任。
// 本类只传输字节；账号 JSON、状态码和会话语义由 MaiChatAuthClient 处理。
package com.kongshang.maichat;

import android.util.Log;

import java.io.IOException;
import java.net.IDN;
import java.net.InetSocketAddress;
import java.net.Proxy;
import java.net.ProxySelector;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.List;

final class MaiChatAuthTransport {
    private static final String LOG_TAG = "MaiChatAuthNetwork";
    private static final int MAX_URL_LENGTH = 2048;
    private static final int MAX_BODY_BYTES = 32 * 1024;
    private static final int MAX_BEARER_LENGTH = 8192;
    private static final int FAILURE_TIMEOUT = 1;
    private static final int FAILURE_CERTIFICATE = 2;
    private static final int FAILURE_TOO_LARGE = 3;
    private static final int FAILURE_SETUP = 4;
    private static final int FAILURE_HEADER_TOO_LARGE = 5;

    static final class Result {
        final int status;
        final byte[] body;
        final int curlCode;
        final int failureKind;

        // 由 MaiChatAuthJni.cpp 构造。
        Result(int status, byte[] body, int curlCode, int failureKind) {
            this.status = status;
            this.body = body;
            this.curlCode = curlCode;
            this.failureKind = failureKind;
        }
    }

    static final class ProxyRoute {
        static final int DIRECT = 0;
        static final int HTTP = 1;
        static final int SOCKS5 = 2;
        final int type;
        final String host;
        final int port;

        ProxyRoute(int type, String host, int port) {
            this.type = type;
            this.host = host;
            this.port = port;
        }
    }

    private static final class NativeLibrary {
        static {
            System.loadLibrary("maichat_agent");
        }

        static void ensureLoaded() { }
    }

    private MaiChatAuthTransport() { }

    static Result perform(URL url, String method, byte[] body, String bearer) throws Exception {
        validateRequest(url, method, body, bearer);
        ProxyRoute route = selectedProxy(url);
        byte[] caPem = new byte[0];
        if ("https".equals(url.getProtocol())) {
            try { caPem = SystemCertificateBundle.currentPem(); }
            catch (Exception failure) {
                throw new IOException("无法读取系统信任证书", failure);
            }
        }
        try {
            NativeLibrary.ensureLoaded();
        } catch (LinkageError failure) {
            throw new IOException("账号网络组件不可用", failure);
        }
        Result result = nativeExecute(url.toExternalForm(), method, body, bearer, caPem,
            route.type, route.host, route.port);
        if (result == null) throw new IOException("账号网络组件未返回结果");
        if (result.curlCode != 0) {
            Log.w(LOG_TAG, "Account transport failed: curl=" + result.curlCode
                + " kind=" + result.failureKind);
            switch (result.failureKind) {
                case FAILURE_TIMEOUT: throw new IOException("连接登录服务超时，请稍后再试");
                case FAILURE_CERTIFICATE: throw new IOException("无法验证登录服务证书");
                case FAILURE_TOO_LARGE: throw new IOException("登录服务响应过大");
                case FAILURE_HEADER_TOO_LARGE: throw new IOException("登录服务响应过大");
                case FAILURE_SETUP: throw new IOException("账号网络组件配置失败");
                default: throw new IOException("登录服务连接失败，请检查网络后重试");
            }
        }
        return result;
    }

    static void validateRequest(URL url, String method, byte[] body, String bearer)
        throws IOException {
        if (url == null || url.toExternalForm().length() > MAX_URL_LENGTH
            || url.getQuery() != null || url.getRef() != null || url.getUserInfo() != null)
            throw new IOException("登录服务地址无效");
        boolean production = "https".equals(url.getProtocol())
            && "ichat.life".equalsIgnoreCase(url.getHost())
            && (url.getPort() == -1 || url.getPort() == 443)
            && url.getPath().startsWith("/api/v1/");
        boolean localTest = BuildConfig.DEBUG && BuildConfig.USE_LOCAL_UI_AUTH
            && "http".equals(url.getProtocol()) && "10.0.2.2".equals(url.getHost())
            && url.getPort() == 18190 && url.getPath().startsWith("/v1/");
        if ((!production && !localTest) || url.getPath().contains(".."))
            throw new IOException("登录服务地址无效");
        if (!("GET".equals(method) && body == null)
            && !("POST".equals(method) && body != null && body.length <= MAX_BODY_BYTES))
            throw new IOException("登录请求数据无效");
        if (bearer == null || bearer.length() > MAX_BEARER_LENGTH)
            throw new IOException("登录凭据无效");
        for (int i = 0; i < bearer.length(); i++) {
            char value = bearer.charAt(i);
            if (value < 33 || value > 126) throw new IOException("登录凭据无效");
        }
        if (!StandardCharsets.US_ASCII.newEncoder().canEncode(url.toExternalForm()))
            throw new IOException("登录服务地址无效");
    }

    static ProxyRoute selectedProxy(URL url) throws IOException {
        ProxySelector selector = ProxySelector.getDefault();
        if (selector == null) return new ProxyRoute(ProxyRoute.DIRECT, "", 0);
        List<Proxy> candidates;
        try {
            candidates = selector.select(url.toURI());
        } catch (Exception failure) {
            throw new IOException("无法读取当前网络代理", failure);
        }
        if (candidates == null || candidates.isEmpty() || candidates.get(0) == null)
            throw new IOException("当前网络代理不可用");
        Proxy selected = candidates.get(0);
        if (selected.type() == Proxy.Type.DIRECT)
            return new ProxyRoute(ProxyRoute.DIRECT, "", 0);
        if (!(selected.address() instanceof InetSocketAddress))
            throw new IOException("当前网络代理地址无效");
        InetSocketAddress address = (InetSocketAddress) selected.address();
        String host = address.getHostString();
        if (host.indexOf(':') == -1) {
            try { host = IDN.toASCII(host); }
            catch (IllegalArgumentException failure) {
                throw new IOException("当前网络代理地址无效", failure);
            }
        }
        if (host.isEmpty() || host.length() > 255 || address.getPort() <= 0)
            throw new IOException("当前网络代理地址无效");
        for (int i = 0; i < host.length(); i++) {
            char value = host.charAt(i);
            if (!((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z')
                || (value >= '0' && value <= '9') || value == '.' || value == '-'
                || value == ':')) throw new IOException("当前网络代理地址无效");
        }
        int type = selected.type() == Proxy.Type.HTTP ? ProxyRoute.HTTP
            : selected.type() == Proxy.Type.SOCKS ? ProxyRoute.SOCKS5 : -1;
        if (type == -1) throw new IOException("当前网络代理类型不受支持");
        return new ProxyRoute(type, host, address.getPort());
    }

    private static native Result nativeExecute(String url, String method, byte[] body,
        String bearer, byte[] caPem, int proxyType, String proxyHost, int proxyPort);
}
