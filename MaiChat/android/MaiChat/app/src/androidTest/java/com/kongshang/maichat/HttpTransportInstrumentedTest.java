// 文件作用：在 Android 运行中的 App 内用本机 HTTP 服务验证通用 curl 的头像下载边界。
// 不访问公网、不请求短信或付费模型；涵盖成功、同协议重定向及超大响应中止。
package com.kongshang.maichat;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertThrows;

import androidx.test.ext.junit.runners.AndroidJUnit4;
import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.Proxy;
import java.net.ProxySelector;
import java.net.ServerSocket;
import java.net.Socket;
import java.net.SocketAddress;
import java.net.URI;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.Test;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
public final class HttpTransportInstrumentedTest {
    @Test
    public void avatarGetReturnsBoundedBody() throws Exception {
        byte[] image = new byte[] {1, 2, 3, 4};
        try (Fixture fixture = new Fixture(1, (path, output) -> send(output, 200, "image/png", image))) {
            MaiChatHttpTransport.Result result = MaiChatHttpTransport.getAvatar(fixture.url("/avatar"));
            assertEquals(200, result.status);
            assertArrayEquals(image, result.body);
        }
    }

    @Test
    public void avatarGetFollowsSameSchemeRedirect() throws Exception {
        try (Fixture fixture = new Fixture(2, (path, output) -> {
            if ("/first".equals(path)) {
                output.write(("HTTP/1.1 302 Found\r\nLocation: /final\r\nContent-Length: 0\r\n"
                    + "Connection: close\r\n\r\n").getBytes(StandardCharsets.US_ASCII));
            } else {
                send(output, 200, "image/png", new byte[] {9});
            }
        })) {
            MaiChatHttpTransport.Result result = MaiChatHttpTransport.getAvatar(fixture.url("/first"));
            assertEquals(200, result.status);
            assertArrayEquals(new byte[] {9}, result.body);
        }
    }

    @Test
    public void crossHostAvatarRedirectSelectsAndUsesNewHostProxy() throws Exception {
        List<String> selectedHosts = Collections.synchronizedList(new ArrayList<>());
        try (Fixture proxy = new Fixture(1, (target, output) -> {
            if (!target.equals("http://127.0.0.2:9/final"))
                throw new IOException("redirect request did not use the selected proxy");
            send(output, 200, "image/png", new byte[] {5});
        }); Fixture first = new Fixture(1, (path, output) -> {
            output.write(("HTTP/1.1 302 Found\r\nLocation: http://127.0.0.2:9/final\r\n"
                + "Content-Length: 0\r\nConnection: close\r\n\r\n")
                .getBytes(StandardCharsets.US_ASCII));
        })) {
            ProxySelector previous = ProxySelector.getDefault();
            ProxySelector.setDefault(new ProxySelector() {
                @Override public List<Proxy> select(URI uri) {
                    selectedHosts.add(uri.getHost());
                    return Collections.singletonList(uri.getHost().equals("127.0.0.2")
                        ? new Proxy(Proxy.Type.HTTP,
                            new InetSocketAddress("127.0.0.1", proxy.port())) : Proxy.NO_PROXY);
                }

                @Override public void connectFailed(URI uri, SocketAddress address, IOException failure) { }
            });
            try {
                MaiChatHttpTransport.Result response = MaiChatHttpTransport.getAvatar(first.url("/first"));
                assertEquals(200, response.status);
                assertArrayEquals(new byte[] {5}, response.body);
                assertEquals(Arrays.asList("127.0.0.1", "127.0.0.2"), selectedHosts);
            } finally {
                ProxySelector.setDefault(previous);
            }
        }
    }

    @Test
    public void avatarRedirectCannotChangeScheme() throws Exception {
        try (Fixture fixture = new Fixture(1, (path, output) -> {
            output.write(("HTTP/1.1 302 Found\r\nLocation: https://127.0.0.1/final\r\n"
                + "Content-Length: 0\r\nConnection: close\r\n\r\n")
                .getBytes(StandardCharsets.US_ASCII));
        })) {
            IOException failure = assertThrows(IOException.class,
                () -> MaiChatHttpTransport.getAvatar(fixture.url("/first")));
            assertEquals("头像地址跳转协议不受支持", failure.getMessage());
        }
    }

    @Test
    public void queryOnlyAvatarRedirectKeepsSignedFilePath() throws Exception {
        try (Fixture fixture = new Fixture(2, (path, output) -> {
            if ("/signed/image.png?old=1".equals(path)) {
                output.write(("HTTP/1.1 302 Found\r\nLocation: ?sig=2\r\nContent-Length: 0\r\n"
                    + "Connection: close\r\n\r\n").getBytes(StandardCharsets.US_ASCII));
            } else if ("/signed/image.png?sig=2".equals(path)) {
                send(output, 200, "image/png", new byte[] {7});
            } else {
                throw new IOException("signed file path changed during redirect: " + path);
            }
        })) {
            MaiChatHttpTransport.Result response =
                MaiChatHttpTransport.getAvatar(fixture.url("/signed/image.png?old=1"));
            assertEquals(200, response.status);
            assertArrayEquals(new byte[] {7}, response.body);
        }
    }

    @Test
    public void oversizedAvatarStopsBeforeDownloadingBody() throws Exception {
        try (Fixture fixture = new Fixture(1, (path, output) -> {
            output.write(("HTTP/1.1 200 OK\r\nContent-Type: image/png\r\n"
                + "Content-Length: 4194305\r\nConnection: close\r\n\r\n")
                .getBytes(StandardCharsets.US_ASCII));
        })) {
            IOException error = assertThrows(IOException.class,
                () -> MaiChatHttpTransport.getAvatar(fixture.url("/too-large")));
            assertEquals("服务端响应过大", error.getMessage());
        }
    }

    @Test
    public void credentialRequestRejectsCleartextBeforeConnecting() throws Exception {
        assertThrows(IOException.class, () -> MaiChatHttpTransport.postCloudCredentials(
            new URL("http://127.0.0.1/credentials"), new byte[] {1}, "example-token"));
    }

    private static void send(OutputStream output, int status, String contentType, byte[] body)
        throws IOException {
        output.write(("HTTP/1.1 " + status + " OK\r\nContent-Type: " + contentType
            + "\r\nContent-Length: " + body.length + "\r\nConnection: close\r\n\r\n")
            .getBytes(StandardCharsets.US_ASCII));
        output.write(body);
    }

    private interface Responder { void respond(String path, OutputStream output) throws IOException; }

    private static final class Fixture implements AutoCloseable {
        private final ServerSocket server;
        private final Thread thread;
        private final AtomicReference<Throwable> error = new AtomicReference<>();

        Fixture(int requests, Responder responder) throws IOException {
            server = new ServerSocket(0, requests, InetAddress.getByName("127.0.0.1"));
            server.setSoTimeout(5000);
            thread = new Thread(() -> {
                for (int index = 0; index < requests; index++) {
                    try (Socket socket = server.accept()) {
                        socket.setSoTimeout(5000);
                        BufferedReader input = new BufferedReader(new InputStreamReader(
                            socket.getInputStream(), StandardCharsets.US_ASCII));
                        String request = input.readLine();
                        if (request == null) throw new IOException("missing request line");
                        String header;
                        while ((header = input.readLine()) != null && !header.isEmpty()) { }
                        responder.respond(request.split(" ")[1], socket.getOutputStream());
                    } catch (Throwable failure) {
                        error.set(failure);
                        return;
                    }
                }
            }, "MaiChat-http-fixture");
            thread.start();
        }

        URL url(String path) throws IOException {
            return new URL("http://127.0.0.1:" + server.getLocalPort() + path);
        }

        int port() { return server.getLocalPort(); }

        @Override public void close() throws Exception {
            thread.join(5000);
            server.close();
            if (thread.isAlive()) throw new IOException("HTTP fixture did not finish");
            if (error.get() != null) throw new IOException("HTTP fixture failed", error.get());
        }
    }
}
