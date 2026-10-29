// 文件作用：用本地 WSS 服务验证 Android 实时语音的 TLS、WebSocket 分帧和 AAC 发送节奏。
// 测试证书只信任 assets/realtime_wss/ca.pem；不连接腾讯付费服务。
package com.kongshang.maichat;

import android.content.Context;
import android.os.SystemClock;

import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;

import org.junit.Test;
import org.junit.runner.RunWith;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.security.KeyStore;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Base64;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

import javax.net.ssl.KeyManagerFactory;
import javax.net.ssl.SSLContext;
import javax.net.ssl.SSLServerSocket;
import javax.net.ssl.SSLSocket;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;

@RunWith(AndroidJUnit4.class)
public class CurlRealtimeSpeechWebSocketInstrumentedTest {
    private static final String ASSET = "realtime_wss/";
    private static final String STS_TOKEN = "local-fixture-token+/=123456";
    private static final MaiChatAuthTransport.ProxyRoute DIRECT =
        new MaiChatAuthTransport.ProxyRoute(MaiChatAuthTransport.ProxyRoute.DIRECT, "", 0);
    private static final String PARTIAL =
        "{\"code\":0,\"sentences\":{\"voice_text_str\":\"测试\"}}";
    private static final String FINAL =
        "{\"code\":0,\"sentences\":{\"voice_text_str\":\"测试完成\"},\"final\":1}";

    // 用自签本机 WSS 服务走真实 JNI/curl：服务端拆分 JSON 帧，同时记录 AAC 到达间隔。
    // 断言转写回调只收到完整 JSON，且音频按录音节奏发送，而不是连接后一次倾倒。
    @Test public void localWssStreamsAacAtRealtimePaceAndReassemblesJsonFrames() throws Exception {
        // 同一握手契约先拒绝缺失/错误 token，再用真实 Stream 验证固定头与 V2 final。
        for (String rejected : new String[] {"", "wrong-fixture-token"}) {
            try (MockWssServer server = new MockWssServer(STS_TOKEN, (input, output, request) -> {
                throw new AssertionError("Unauthorized WebSocket was upgraded");
            })) {
                CountDownLatch failed = new CountDownLatch(1);
                AtomicInteger messages = new AtomicInteger();
                CurlRealtimeSpeechWebSocket socket = new CurlRealtimeSpeechWebSocket(
                    "wss://localhost:" + server.port() + "/asr/v2/123", rejected, ca(), DIRECT,
                    new CurlRealtimeSpeechWebSocket.Listener() {
                        @Override public void message(String value) { messages.incrementAndGet(); }
                        @Override public void failed(int code) { failed.countDown(); }
                        @Override public void closed() { }
                    });
                try {
                    assertTrue("Missing/wrong STS header was accepted", failed.await(8, TimeUnit.SECONDS));
                    assertEquals(0, messages.get());
                    server.assertSucceeded();
                } finally { socket.cancel(); }
            }
        }
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        File audio = new File(context.getCacheDir(), "wss-synthetic-adts.aac");
        writeAdts(audio, 12);
        List<Long> arrivals = new ArrayList<>();
        AtomicReference<String> requestLine = new AtomicReference<>();
        try (MockWssServer server = new MockWssServer(STS_TOKEN, (input, output, request) -> {
            // 首包使客户端进入 ready；随后在音频到来时故意把识别结果拆成两帧。
            requestLine.set(request);
            sendText(output, "{\"code\":0}");
            boolean partialSent = false;
            while (true) {
                Frame frame = readFrame(input);
                assertNotNull("Client disconnected before end marker", frame);
                if (frame.opcode == 2) {
                    arrivals.add(SystemClock.elapsedRealtime());
                    assertTrue("Expected complete AAC frames", frame.payload.length % 39 == 0);
                    if (!partialSent) {
                        byte[] message = PARTIAL.getBytes(StandardCharsets.UTF_8);
                        int split = message.length / 2;
                        sendFrame(output, 1, false, message, 0, split);
                        sendFrame(output, 0, true, message, split, message.length - split);
                        partialSent = true;
                    }
                } else if (frame.opcode == 1) {
                    assertEquals("{\"type\":\"end\"}", new String(frame.payload, StandardCharsets.UTF_8));
                    // V2 结束时仍可修正文；超过旧的一秒超时后必须交付 final 而非 partial。
                    SystemClock.sleep(1300);
                    sendText(output, FINAL);
                    return;
                }
            }
        })) {
            // 客户端沿产品同一条 Stream 路径发送本地 AAC，结束标记仍由产品代码发出。
            CountDownLatch done = new CountDownLatch(1);
            AtomicReference<String> text = new AtomicReference<>();
            List<String> partials = new ArrayList<>();
            List<String> events = new ArrayList<>();
            String url = "wss://localhost:" + server.port() + "/asr/v2/123?signature=a%2Bb%3D";
            TencentRealtimeSpeechStream stream = new TencentRealtimeSpeechStream(
                audio, url, STS_TOKEN, ca(), DIRECT, new TencentRealtimeSpeechStream.Listener() {
                    @Override public void partial(String value) { synchronized (partials) { partials.add(value); } }
                    @Override public void diagnostic(String value) { synchronized (events) { events.add(value); } }
                    @Override public void finished(String value) { text.set(value); done.countDown(); }
                    @Override public void failed() { done.countDown(); }
                });
            try {
                stream.endAudio();
                assertTrue("ASR stream did not finish", done.await(10, TimeUnit.SECONDS));
                assertEquals("测试完成", text.get());
                synchronized (partials) { assertTrue("No partial transcript", partials.contains("测试")); }
                synchronized (events) {
                    assertTrue("No server final diagnostic",
                        events.stream().anyMatch(value -> value.startsWith("asr-server-final")));
                }
            } finally { stream.cancel(); }
            // 服务端收到的签名查询串与发送节奏同时核对，避免“能连上但不能实时识别”。
            server.assertSucceeded();
            assertEquals("GET /asr/v2/123?signature=a%2Bb%3D HTTP/1.1", requestLine.get());
            assertTrue("Expected multiple timed AAC packets", arrivals.size() >= 3);
            for (int index = 1; index < arrivals.size(); ++index) {
                assertTrue("AAC packets arrived too quickly: " + arrivals,
                    arrivals.get(index) - arrivals.get(index - 1) >= 100);
            }
        } finally { audio.delete(); }
    }

    @Test public void localWssRejectsCertificateForWrongHostname() throws Exception {
        try (MockWssServer server = new MockWssServer((input, output, request) -> { })) {
            CountDownLatch failed = new CountDownLatch(1);
            AtomicInteger error = new AtomicInteger();
            CurlRealtimeSpeechWebSocket socket = new CurlRealtimeSpeechWebSocket(
                "wss://127.0.0.1:" + server.port() + "/wrong-host", ca(), DIRECT,
                new CurlRealtimeSpeechWebSocket.Listener() {
                    @Override public void message(String value) { }
                    @Override public void failed(int code) { error.set(code); failed.countDown(); }
                    @Override public void closed() { failed.countDown(); }
                });
            try {
                assertTrue("Wrong hostname was not rejected", failed.await(8, TimeUnit.SECONDS));
                assertEquals("TLS hostname check should fail", 60, error.get());
            } finally { socket.cancel(); }
        }
    }

    @Test public void localWssRepliesToPingAndReportsClose() throws Exception {
        try (MockWssServer server = new MockWssServer((input, output, request) -> {
            byte[] ping = "check".getBytes(StandardCharsets.US_ASCII);
            sendFrame(output, 9, true, ping, 0, ping.length);
            Frame pong = readFrame(input);
            assertNotNull("No pong reply", pong);
            assertEquals(10, pong.opcode);
            assertEquals("check", new String(pong.payload, StandardCharsets.US_ASCII));
            sendFrame(output, 8, true, new byte[] {3, (byte) 0xe8}, 0, 2);
        })) {
            CountDownLatch closed = new CountDownLatch(1);
            AtomicInteger failure = new AtomicInteger();
            CurlRealtimeSpeechWebSocket socket = new CurlRealtimeSpeechWebSocket(
                "wss://localhost:" + server.port() + "/control", ca(), DIRECT,
                new CurlRealtimeSpeechWebSocket.Listener() {
                    @Override public void message(String value) { }
                    @Override public void failed(int code) { failure.set(code); closed.countDown(); }
                    @Override public void closed() { closed.countDown(); }
                });
            try {
                assertTrue("Close frame was not reported", closed.await(8, TimeUnit.SECONDS));
                assertEquals("Unexpected WebSocket failure", 0, failure.get());
                server.assertSucceeded();
            } finally { socket.cancel(); }
        }
    }

    @Test public void localWssKeepsOutboundQueueBoundedDuringHandshake() throws Exception {
        try (MockWssServer server = new MockWssServer(1000, (input, output, request) -> { })) {
            CountDownLatch notified = new CountDownLatch(1);
            CurlRealtimeSpeechWebSocket socket = new CurlRealtimeSpeechWebSocket(
                "wss://localhost:" + server.port() + "/queue", ca(), DIRECT,
                new CurlRealtimeSpeechWebSocket.Listener() {
                    @Override public void message(String value) { notified.countDown(); }
                    @Override public void failed(int code) { notified.countDown(); }
                    @Override public void closed() { notified.countDown(); }
                });
            try {
                assertTrue(socket.sendBinary(new byte[60 * 1024]));
                assertTrue(socket.sendBinary(new byte[60 * 1024]));
                assertFalse("Outbound queue exceeded 128 KiB", socket.sendBinary(new byte[9 * 1024]));
            } finally { socket.cancel(); }
            assertFalse("Cancelled transport delivered a callback", notified.await(1200, TimeUnit.MILLISECONDS));
        }
    }

    @Test public void localWssUsesHttpConnectProxy() throws Exception {
        try (MockWssServer server = new MockWssServer((input, output, request) -> {
            sendText(output, "{\"code\":0}");
            sendFrame(output, 8, true, new byte[] {3, (byte) 0xe8}, 0, 2);
        }); MockConnectProxy proxy = new MockConnectProxy(server.port())) {
            CountDownLatch closed = new CountDownLatch(1);
            AtomicReference<String> message = new AtomicReference<>();
            AtomicInteger failure = new AtomicInteger();
            MaiChatAuthTransport.ProxyRoute route = new MaiChatAuthTransport.ProxyRoute(
                MaiChatAuthTransport.ProxyRoute.HTTP, "127.0.0.1", proxy.port());
            CurlRealtimeSpeechWebSocket socket = new CurlRealtimeSpeechWebSocket(
                "wss://localhost:" + server.port() + "/proxy", ca(), route,
                new CurlRealtimeSpeechWebSocket.Listener() {
                    @Override public void message(String value) { message.set(value); }
                    @Override public void failed(int code) { failure.set(code); closed.countDown(); }
                    @Override public void closed() { closed.countDown(); }
                });
            try {
                assertTrue("Proxied WebSocket did not close", closed.await(8, TimeUnit.SECONDS));
                assertEquals(0, failure.get());
                assertEquals("{\"code\":0}", message.get());
                assertEquals("CONNECT localhost:" + server.port() + " HTTP/1.1", proxy.connectLine());
                server.assertSucceeded();
            } finally { socket.cancel(); }
        }
    }

    private static byte[] ca() throws IOException {
        try (InputStream input = InstrumentationRegistry.getInstrumentation().getContext()
                .getAssets().open(ASSET + "ca.pem")) {
            return readAll(input);
        }
    }

    private static void writeAdts(File file, int frames) throws IOException {
        try (FileOutputStream output = new FileOutputStream(file)) {
            for (int index = 0; index < frames; ++index) {
                output.write(new byte[] {(byte) 0xff, (byte) 0xf1, 0x60, 0x40, 0x04, (byte) 0xff,
                                         (byte) 0xfc});
                output.write(new byte[32]);
            }
        }
    }

    private static byte[] readAll(InputStream input) throws IOException {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        byte[] buffer = new byte[4096];
        for (int count; (count = input.read(buffer)) >= 0; ) bytes.write(buffer, 0, count);
        return bytes.toByteArray();
    }

    private static String readLine(InputStream input) throws IOException {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        for (int value; (value = input.read()) >= 0; ) {
            if (value == '\n') break;
            if (value != '\r') bytes.write(value);
            if (bytes.size() > 8192) throw new IOException("WebSocket header too long");
        }
        return bytes.toString("US-ASCII");
    }

    private static Frame readFrame(InputStream input) throws IOException {
        int first = input.read();
        if (first < 0) return null;
        int second = input.read();
        if (second < 0) throw new IOException("Truncated WebSocket frame");
        assertTrue("Client frame must be masked", (second & 0x80) != 0);
        int length = second & 0x7f;
        if (length == 126) length = (input.read() << 8) | input.read();
        else if (length == 127) throw new IOException("Unexpected large frame");
        if (length < 0 || length > 128 * 1024) throw new IOException("Unexpected frame length");
        byte[] mask = new byte[4];
        for (int offset = 0; offset < mask.length; ) {
            int count = input.read(mask, offset, mask.length - offset);
            if (count < 0) throw new IOException("Truncated WebSocket mask");
            offset += count;
        }
        byte[] payload = new byte[length];
        for (int offset = 0; offset < length; ) {
            int count = input.read(payload, offset, length - offset);
            if (count < 0) throw new IOException("Truncated WebSocket payload");
            offset += count;
        }
        for (int index = 0; index < payload.length; ++index) payload[index] ^= mask[index & 3];
        return new Frame(first & 0x0f, payload);
    }

    private static void sendText(OutputStream output, String text) throws IOException {
        byte[] payload = text.getBytes(StandardCharsets.UTF_8);
        sendFrame(output, 1, true, payload, 0, payload.length);
    }

    private static void sendFrame(OutputStream output, int opcode, boolean finalFrame,
                                  byte[] payload, int offset, int length) throws IOException {
        output.write((finalFrame ? 0x80 : 0) | opcode);
        if (length < 126) output.write(length);
        else {
            output.write(126);
            output.write((length >> 8) & 255);
            output.write(length & 255);
        }
        output.write(payload, offset, length);
        output.flush();
    }

    private static final class Frame {
        final int opcode;
        final byte[] payload;
        Frame(int opcode, byte[] payload) { this.opcode = opcode; this.payload = payload; }
    }

    private interface ServerAction {
        void run(InputStream input, OutputStream output, String request) throws Exception;
    }

    private static final class MockWssServer implements AutoCloseable {
        private final SSLServerSocket server;
        private final Thread thread;
        private final AtomicReference<Throwable> failure = new AtomicReference<>();
        private final String requiredToken;

        MockWssServer(ServerAction action) throws Exception { this(0, null, action); }

        MockWssServer(long handshakeDelayMs, ServerAction action) throws Exception {
            this(handshakeDelayMs, null, action);
        }

        MockWssServer(String requiredToken, ServerAction action) throws Exception {
            this(0, requiredToken, action);
        }

        MockWssServer(long handshakeDelayMs, String requiredToken, ServerAction action) throws Exception {
            this.requiredToken = requiredToken;
            KeyStore store = KeyStore.getInstance("PKCS12");
            char[] password = "local-test-only".toCharArray();
            try (InputStream input = InstrumentationRegistry.getInstrumentation().getContext()
                    .getAssets().open(ASSET + "server.p12")) {
                store.load(input, password);
            }
            KeyManagerFactory keys = KeyManagerFactory.getInstance(KeyManagerFactory.getDefaultAlgorithm());
            keys.init(store, password);
            SSLContext tls = SSLContext.getInstance("TLS");
            tls.init(keys.getKeyManagers(), null, null);
            server = (SSLServerSocket) tls.getServerSocketFactory().createServerSocket(
                0, 1, InetAddress.getByName("127.0.0.1"));
            server.setSoTimeout(8000);
            thread = new Thread(() -> serve(handshakeDelayMs, action), "MaiChat-local-wss");
            thread.start();
        }

        int port() { return server.getLocalPort(); }

        private void serve(long handshakeDelayMs, ServerAction action) {
            try (SSLSocket socket = (SSLSocket) server.accept()) {
                socket.setSoTimeout(8000);
                InputStream input = socket.getInputStream();
                OutputStream output = socket.getOutputStream();
                String request = readLine(input);
                String key = null;
                String token = null;
                for (String line; !(line = readLine(input)).isEmpty(); ) {
                    if (line.regionMatches(true, 0, "Sec-WebSocket-Key:", 0, 18))
                        key = line.substring(18).trim();
                    if (line.regionMatches(true, 0, "X-TC-Token:", 0, 11))
                        token = line.substring(11).trim();
                }
                if (requiredToken != null && !requiredToken.equals(token)) {
                    output.write("HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n"
                        .getBytes(StandardCharsets.US_ASCII));
                    output.flush();
                    return;
                }
                if (key == null) throw new IOException("Missing WebSocket key");
                String accept = Base64.getEncoder().encodeToString(MessageDigest.getInstance("SHA-1")
                    .digest((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11")
                        .getBytes(StandardCharsets.US_ASCII)));
                if (handshakeDelayMs > 0) Thread.sleep(handshakeDelayMs);
                output.write(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                    + "Connection: Upgrade\r\nSec-WebSocket-Accept: " + accept + "\r\n\r\n")
                    .getBytes(StandardCharsets.US_ASCII));
                output.flush();
                action.run(input, output, request);
            } catch (Throwable error) { failure.set(error); }
        }

        void assertSucceeded() throws Exception {
            thread.join(9000);
            assertFalse("Mock WSS server did not stop", thread.isAlive());
            if (failure.get() != null) throw new AssertionError("Mock WSS server failed", failure.get());
        }

        @Override public void close() throws Exception {
            server.close();
            thread.join(9000);
        }
    }

    private static final class MockConnectProxy implements AutoCloseable {
        private final ServerSocket server;
        private final Thread thread;
        private final int targetPort;
        private final AtomicReference<String> request = new AtomicReference<>();

        MockConnectProxy(int targetPort) throws IOException {
            this.targetPort = targetPort;
            server = new ServerSocket(0, 1, InetAddress.getByName("127.0.0.1"));
            server.setSoTimeout(8000);
            thread = new Thread(this::serve, "MaiChat-local-connect-proxy");
            thread.start();
        }

        int port() { return server.getLocalPort(); }
        String connectLine() { return request.get(); }

        private void serve() {
            try (Socket client = server.accept();
                 Socket upstream = new Socket(InetAddress.getByName("127.0.0.1"), targetPort)) {
                client.setSoTimeout(8000);
                upstream.setSoTimeout(8000);
                InputStream fromClient = client.getInputStream();
                request.set(readLine(fromClient));
                for (String line; !(line = readLine(fromClient)).isEmpty(); ) { }
                client.getOutputStream().write("HTTP/1.1 200 Connection established\r\n\r\n"
                    .getBytes(StandardCharsets.US_ASCII));
                client.getOutputStream().flush();
                Thread downlink = new Thread(() -> copy(upstream, client), "MaiChat-local-connect-downlink");
                downlink.start();
                copy(client, upstream);
                downlink.join(8000);
            } catch (Exception ignored) { }
        }

        private static void copy(Socket source, Socket destination) {
            try {
                InputStream input = source.getInputStream();
                OutputStream output = destination.getOutputStream();
                byte[] bytes = new byte[4096];
                for (int count; (count = input.read(bytes)) >= 0; ) {
                    output.write(bytes, 0, count);
                    output.flush();
                }
                destination.shutdownOutput();
            } catch (IOException ignored) { }
        }

        @Override public void close() throws Exception {
            server.close();
            thread.join(9000);
        }
    }
}
