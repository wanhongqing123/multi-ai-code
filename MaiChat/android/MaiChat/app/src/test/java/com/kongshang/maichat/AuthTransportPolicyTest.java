// 文件作用：验证账号原生传输只接受预定地址、受限请求及系统代理。
// 测试不连接网络，也不发送短信。
package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertThrows;

import org.junit.Test;

import java.io.IOException;
import java.net.InetSocketAddress;
import java.net.Proxy;
import java.net.ProxySelector;
import java.net.SocketAddress;
import java.net.URI;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.Collections;
import java.util.List;

public final class AuthTransportPolicyTest {
    @Test public void acceptsOnlyProductionAccountEndpointAndBoundedRequest() throws Exception {
        URL account = new URL("https://ichat.life/api/v1/auth/sms/challenge");
        byte[] request = "{\"phone\":\"123\"}".getBytes(StandardCharsets.UTF_8);
        MaiChatAuthTransport.validateRequest(account, "POST", request, "");
        for (String value : new String[] {
                "http://ichat.life/api/v1/auth/sms/challenge",
                "https://www.ichat.life/api/v1/auth/sms/challenge",
                "https://ichat.life:8443/api/v1/auth/sms/challenge",
                "https://ichat.life/legal/terms.html",
                "https://ichat.life/api/v1/../admin",
                "https://ichat.life/api/v1/auth/me?token=secret" }) {
            assertThrows(IOException.class, () -> MaiChatAuthTransport.validateRequest(
                new URL(value), "GET", null, ""));
        }
        assertThrows(IOException.class, () -> MaiChatAuthTransport.validateRequest(
            account, "POST", new byte[32 * 1024 + 1], ""));
        assertThrows(IOException.class, () -> MaiChatAuthTransport.validateRequest(
            account, "POST", request, "token\r\nInjected: yes"));
        assertThrows(IOException.class, () -> MaiChatAuthTransport.validateRequest(
            account, "GET", request, ""));
    }

    @Test public void sessionSyncAllowsOnlyBoundedPollAndMessagePayloads() throws Exception {
        URL poll = new URL("https://ichat.life/api/v1/session-sync/poll?wait_ms=10000");
        MaiChatAuthTransport.validateRequest(poll, "GET", null, "account-token");
        URL send = new URL("https://ichat.life/api/v1/session-sync/send");
        MaiChatAuthTransport.validateRequest(send, "POST", new byte[64 * 1024],
            "account-token");
        assertThrows(IOException.class, () -> MaiChatAuthTransport.validateRequest(
            send, "POST", new byte[96 * 1024 + 1], "account-token"));
        for (String url : new String[] {
                "https://ichat.life/api/v1/session-sync/poll?wait_ms=20001",
                "https://ichat.life/api/v1/session-sync/poll?wait_ms=1000&token=secret",
                "https://ichat.life/api/v1/session-sync/send?wait_ms=1" }) {
            assertThrows(IOException.class, () -> MaiChatAuthTransport.validateRequest(
                new URL(url), "GET", null, "account-token"));
        }
    }

    @Test public void usesSelectedSystemProxyInsteadOfSilentlyConnectingDirect() throws Exception {
        ProxySelector original = ProxySelector.getDefault();
        Proxy proxy = new Proxy(Proxy.Type.HTTP,
            InetSocketAddress.createUnresolved("proxy.example", 8888));
        ProxySelector.setDefault(new SingleProxySelector(proxy));
        try {
            MaiChatAuthTransport.ProxyRoute route = MaiChatAuthTransport.selectedProxy(
                new URL("https://ichat.life/api/v1/auth/me"));
            assertEquals(MaiChatAuthTransport.ProxyRoute.HTTP, route.type);
            assertEquals("proxy.example", route.host);
            assertEquals(8888, route.port);
        } finally {
            ProxySelector.setDefault(original);
        }
    }

    private static final class SingleProxySelector extends ProxySelector {
        private final Proxy proxy;

        SingleProxySelector(Proxy proxy) { this.proxy = proxy; }

        @Override public List<Proxy> select(URI uri) {
            return Collections.singletonList(proxy);
        }

        @Override public void connectFailed(URI uri, SocketAddress address,
            IOException error) { }
    }
}
