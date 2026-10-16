// 文件作用：为 Android 自有的一次性 HTTP 请求提供统一的 libcurl/OpenSSL 传输入口。
// 账号、头像及旧云凭据调用方各自校验业务地址；本类只负责传输、大小限制、系统证书与代理。
package com.kongshang.maichat;

import android.util.Log;
import java.io.IOException;
import java.net.MalformedURLException;
import java.net.URI;
import java.net.URISyntaxException;
import java.net.URL;

final class MaiChatHttpTransport {
    private static final String LOG_TAG = "MaiChatHttp";
    private static final int MAX_URL_LENGTH = 2048;
    private static final int MAX_REQUEST_BYTES = 32 * 1024;
    private static final int MAX_BEARER_LENGTH = 8192;
    private static final int MAX_RESPONSE_BYTES = 4 * 1024 * 1024;
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
        final String redirectLocation;

        // 由 MaiChatAuthJni.cpp 构造；业务层据 HTTP 状态决定是否使用正文。
        Result(int status, byte[] body, int curlCode, int failureKind, String redirectLocation) {
            this.status = status;
            this.body = body;
            this.curlCode = curlCode;
            this.failureKind = failureKind;
            this.redirectLocation = redirectLocation;
        }
    }

    private static final class NativeLibrary {
        static { System.loadLibrary("maichat_agent"); }
        static void ensureLoaded() { }
    }

    private MaiChatHttpTransport() { }

    // 作用：下载联系人头像。每跳重新选 Android 系统代理，只允许同协议的 GET 重定向。
    static Result getAvatar(URL url) throws IOException {
        URL current = url;
        for (int redirects = 0; redirects <= 5; redirects++) {
            Result response = perform(current, "GET", null, "", MAX_RESPONSE_BYTES, 5_000, 0, 8);
            if (!isRedirect(response.status) || response.redirectLocation == null)
                return response;
            if (redirects == 5) throw new IOException("头像地址跳转次数过多");
            current = resolveAvatarRedirect(current, response.redirectLocation);
        }
        throw new IOException("头像地址跳转次数过多");
    }

    // 作用：从用户配置的 HTTPS 云服务取得短响应；禁重定向，Bearer 不转发到另一主机。
    static Result postCloudCredentials(URL url, byte[] body, String bearer) throws IOException {
        if (!"https".equals(url.getProtocol())) throw new IOException("云端密钥服务地址必须使用 HTTPS");
        try {
            return perform(url, "POST", body, bearer, 16_384, 10_000, 30_000, 20);
        } catch (IOException failure) {
            if ("服务端响应过大".equals(failure.getMessage()))
                throw new IOException("云端密钥响应过大", failure);
            throw failure;
        }
    }

    private static boolean isRedirect(int status) {
        return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
    }

    private static URL resolveAvatarRedirect(URL previous, String location) throws IOException {
        try {
            // Java 的 URI.resolve("?sig=...") 会丢掉文件名；查询串跳转必须沿用原路径。
            String previousText = previous.toExternalForm();
            int queryStart = previousText.indexOf('?');
            URI resolved = location.startsWith("?")
                ? new URI(previousText.substring(0, queryStart < 0 ? previousText.length() : queryStart)
                    + location)
                : previous.toURI().resolve(new URI(location));
            URL next = resolved.toURL();
            if (!previous.getProtocol().equals(next.getProtocol()))
                throw new IOException("头像地址跳转协议不受支持");
            validateRequest(next, "GET", null, "");
            return next;
        } catch (URISyntaxException | MalformedURLException failure) {
            throw new IOException("头像地址跳转无效", failure);
        }
    }

    private static Result perform(URL url, String method, byte[] body, String bearer,
                                  int responseLimit, int connectTimeoutMs, int totalTimeoutMs,
                                  int lowSpeedSeconds) throws IOException {
        validateRequest(url, method, body, bearer);
        MaiChatAuthTransport.ProxyRoute route = MaiChatAuthTransport.selectedProxy(url);
        byte[] caPem = new byte[0];
        if ("https".equals(url.getProtocol())) {
            try { caPem = SystemCertificateBundle.currentPem(); }
            catch (Exception failure) { throw new IOException("无法读取系统信任证书", failure); }
        }
        try { NativeLibrary.ensureLoaded(); }
        catch (LinkageError failure) { throw new IOException("网络组件不可用", failure); }
        Result result = nativeExecute(url.toExternalForm(), method, body, bearer, caPem,
            route.type, route.host, route.port, responseLimit, connectTimeoutMs,
            totalTimeoutMs, lowSpeedSeconds);
        if (result == null) throw new IOException("网络组件未返回结果");
        // 旧云凭据实现先看 HTTP 状态，非 200 响应不读正文。即使错误页过大，也保留 401 等原始状态。
        if ("POST".equals(method) && result.status >= 100 && result.status != 200
            && (result.curlCode == 0 || result.failureKind == FAILURE_TOO_LARGE))
            return new Result(result.status, new byte[0], 0, 0, null);
        if (result.curlCode == 0) return result;
        Log.w(LOG_TAG, "HTTP transport failed: curl=" + result.curlCode
            + " kind=" + result.failureKind);
        switch (result.failureKind) {
            case FAILURE_TIMEOUT: throw new IOException("网络请求超时");
            case FAILURE_CERTIFICATE: throw new IOException("无法验证服务端证书");
            case FAILURE_TOO_LARGE: throw new IOException("服务端响应过大");
            case FAILURE_HEADER_TOO_LARGE: throw new IOException("服务端响应头过大");
            case FAILURE_SETUP: throw new IOException("网络组件配置失败");
            default: throw new IOException("网络连接失败");
        }
    }

    static void validateRequest(URL url, String method, byte[] body, String bearer) throws IOException {
        if (url == null || url.toExternalForm().length() > MAX_URL_LENGTH || url.getHost().isEmpty()
            || url.getUserInfo() != null || url.getRef() != null
            || !("http".equals(url.getProtocol()) || "https".equals(url.getProtocol())))
            throw new IOException("网络地址无效");
        for (int index = 0; index < url.toExternalForm().length(); index++)
            if (Character.isISOControl(url.toExternalForm().charAt(index)))
                throw new IOException("网络地址无效");
        if (!("GET".equals(method) && body == null)
            && !("POST".equals(method) && body != null && body.length <= MAX_REQUEST_BYTES))
            throw new IOException("网络请求数据无效");
        if (bearer == null || bearer.length() > MAX_BEARER_LENGTH)
            throw new IOException("服务令牌无效");
        for (int index = 0; index < bearer.length(); index++) {
            char value = bearer.charAt(index);
            if (value < 33 || value > 126) throw new IOException("服务令牌无效");
        }
    }

    private static native Result nativeExecute(String url, String method, byte[] body,
        String bearer, byte[] caPem, int proxyType, String proxyHost, int proxyPort,
        int responseLimit, int connectTimeoutMs, int totalTimeoutMs, int lowSpeedSeconds);
}
