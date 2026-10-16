// 文件作用：锁定账号 TLS 回退的域名、端口、代理和单次重试边界。
// 此测试不连接网络，不创建账号，也不发送验证码。
package com.kongshang.maichat;

import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

import java.net.HttpURLConnection;
import java.net.ConnectException;
import java.net.InetSocketAddress;
import java.net.Proxy;
import java.net.ProxySelector;
import java.net.SocketAddress;
import java.net.SocketException;
import java.net.URI;
import java.net.URL;
import java.util.Collections;
import java.util.List;

public final class AuthNoSniFallbackPolicyTest {
    @Test public void onlyDirectProductionAuthHostCanUseFallbackOnce() throws Exception {
        ProxySelector original = ProxySelector.getDefault();
        ProxySelector.setDefault(new SingleProxySelector(Proxy.NO_PROXY));
        try {
            URL accountUrl = new URL("https://ichat.life/api/v1/auth/me");
            FakeConnection directConnection = new FakeConnection(accountUrl, false);
            assertTrue(MaiChatAuthClient.canUseNoSniFallback(accountUrl, directConnection, false));
            assertFalse(MaiChatAuthClient.canUseNoSniFallback(accountUrl, directConnection, true));
            assertFalse(MaiChatAuthClient.canUseNoSniFallback(accountUrl,
                new FakeConnection(accountUrl, true), false));
            for (String value : new String[] {
                    "http://ichat.life/api/v1/auth/me",
                    "https://ichat.life/legal/terms.html",
                    "https://www.ichat.life/api/v1/auth/me",
                    "https://ichat.life.example/api/v1/auth/me",
                    "https://ichat.life:8443/api/v1/auth/me" }) {
                URL other = new URL(value);
                assertFalse(MaiChatAuthClient.canUseNoSniFallback(other,
                    new FakeConnection(other, false), false));
            }
        } finally {
            ProxySelector.setDefault(original);
        }
    }

    @Test public void configuredProxyBlocksFallbackEvenIfConnectionReportsDirect()
        throws Exception {
        ProxySelector original = ProxySelector.getDefault();
        ProxySelector.setDefault(new SingleProxySelector(new Proxy(Proxy.Type.HTTP,
            new InetSocketAddress("127.0.0.1", 8888))));
        try {
            URL accountUrl = new URL("https://ichat.life/api/v1/auth/me");
            assertFalse(MaiChatAuthClient.canUseNoSniFallback(accountUrl,
                new FakeConnection(accountUrl, false), false));
        } finally {
            ProxySelector.setDefault(original);
        }
    }

    @Test public void onlyConnectionResetQualifiesAsTransportFailure() {
        assertTrue(MaiChatAuthClient.isConnectionReset(new SocketException("Connection reset")));
        assertTrue(MaiChatAuthClient.isConnectionReset(new SocketException("Connection reset by peer")));
        assertFalse(MaiChatAuthClient.isConnectionReset(new ConnectException("Connection refused")));
        assertFalse(MaiChatAuthClient.isConnectionReset(new SocketException("Network is unreachable")));
        assertFalse(MaiChatAuthClient.isConnectionReset(new SocketException("Broken pipe")));
    }

    private static final class SingleProxySelector extends ProxySelector {
        private final Proxy proxy;

        SingleProxySelector(Proxy proxy) { this.proxy = proxy; }

        @Override public List<Proxy> select(URI uri) {
            return Collections.singletonList(proxy);
        }

        @Override public void connectFailed(URI uri, SocketAddress address,
            java.io.IOException error) { }
    }

    private static final class FakeConnection extends HttpURLConnection {
        private final boolean proxied;

        FakeConnection(URL address, boolean proxied) {
            super(address);
            this.proxied = proxied;
        }

        @Override public boolean usingProxy() { return proxied; }

        @Override public void disconnect() { }

        @Override public void connect() { }
    }
}
