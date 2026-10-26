package com.kongshang.maichat;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import android.content.Context;

import androidx.test.core.app.ApplicationProvider;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;

import java.io.BufferedReader;
import java.io.File;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.net.InetAddress;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.KeyStore;
import java.util.UUID;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

import javax.net.ssl.KeyManagerFactory;
import javax.net.ssl.SSLContext;
import javax.net.ssl.SSLServerSocket;
import javax.net.ssl.SSLSocket;

import org.junit.Test;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
public final class AttachmentDownloadInstrumentedTest {
    @Test public void localTlsDownloadPublishesCompleteFileAndRejectsOversize() throws Exception {
        byte[] body = new byte[] {1, 3, 5, 7};
        try (Fixture fixture = new Fixture(output -> send(output, body))) {
            File target = target("complete");
            ResultWaiter result = download(fixture, target, body.length, fixture.trustPem);
            MaiChatAttachmentDownload.Result completed = result.await();
            assertTrue("status=" + completed.status + " curl=" + completed.curlCode
                + " http=" + completed.httpStatus, completed.ok());
            assertArrayEquals(body, Files.readAllBytes(target.toPath()));
        }
        try (Fixture fixture = new Fixture(output -> send(output, body))) {
            File target = target("oversize");
            ResultWaiter result = download(fixture, target, body.length - 1, fixture.trustPem);
            assertEquals(MaiChatAttachmentDownload.TOO_LARGE, result.await().status);
            assertFalse(target.exists());
        }
    }

    @Test public void untrustedTlsServerDoesNotPublishFile() throws Exception {
        try (Fixture fixture = new Fixture(output -> send(output, new byte[] {9}))) {
            File target = target("untrusted");
            ResultWaiter result = download(fixture, target, 1024, new byte[0]);
            assertFalse(result.await().ok());
            assertFalse(target.exists());
        }
    }

    @Test public void cancellationKeepsExistingFileIntact() throws Exception {
        CountDownLatch bodyStarted = new CountDownLatch(1);
        try (Fixture fixture = new Fixture(output -> {
            output.write(("HTTP/1.1 200 OK\r\nContent-Length: 1048576\r\n"
                + "Connection: close\r\n\r\n").getBytes(StandardCharsets.US_ASCII));
            byte[] chunk = new byte[4096];
            for (int i = 0; i < 256; i++) {
                output.write(chunk);
                output.flush();
                if (i == 0) bodyStarted.countDown();
                try { Thread.sleep(20); } catch (InterruptedException ignored) { return; }
            }
        })) {
            File target = target("cancel");
            byte[] previous = new byte[] {42};
            Files.write(target.toPath(), previous);
            MaiChatAttachmentDownload.Scope scope = new MaiChatAttachmentDownload.Scope();
            ResultWaiter result = download(fixture, target, 2L * 1024 * 1024, fixture.trustPem, scope);
            assertTrue(bodyStarted.await(5, TimeUnit.SECONDS));
            scope.cancel();
            assertEquals(MaiChatAttachmentDownload.CANCELED, result.await().status);
            assertArrayEquals(previous, Files.readAllBytes(target.toPath()));
        }
    }

    private static ResultWaiter download(Fixture fixture, File target, long limit, byte[] trustPem) {
        return download(fixture, target, limit, trustPem, new MaiChatAttachmentDownload.Scope());
    }

    private static ResultWaiter download(Fixture fixture, File target, long limit, byte[] trustPem,
                                         MaiChatAttachmentDownload.Scope scope) {
        ResultWaiter waiter = new ResultWaiter();
        new MaiChatAttachmentDownload.NativeTransport(() -> trustPem).download(
            fixture.url(), target, limit, scope, waiter::finish);
        return waiter;
    }

    private static File target(String name) throws IOException {
        Context context = ApplicationProvider.getApplicationContext();
        File directory = new File(context.getFilesDir(), "attachment-download-test");
        if (!directory.isDirectory() && !directory.mkdirs()) throw new IOException("mkdir failed");
        File target = new File(directory, name + "-" + UUID.randomUUID() + ".bin");
        if (target.exists() && !target.delete()) throw new IOException("cannot clear test target");
        return target;
    }

    private static void send(OutputStream output, byte[] body) throws IOException {
        output.write(("HTTP/1.1 200 OK\r\nContent-Length: " + body.length + "\r\n"
            + "Connection: close\r\n\r\n").getBytes(StandardCharsets.US_ASCII));
        output.write(body);
        output.flush();
    }

    private static final class ResultWaiter {
        private final CountDownLatch done = new CountDownLatch(1);
        private final AtomicReference<MaiChatAttachmentDownload.Result> value = new AtomicReference<>();
        void finish(MaiChatAttachmentDownload.Result result) { value.set(result); done.countDown(); }
        MaiChatAttachmentDownload.Result await() throws Exception {
            assertTrue("attachment download did not finish", done.await(12, TimeUnit.SECONDS));
            return value.get();
        }
    }

    private interface Response { void write(OutputStream output) throws Exception; }

    private static final class Fixture implements AutoCloseable {
        final byte[] trustPem;
        private final SSLServerSocket server;
        private final Thread worker;
        private final AtomicReference<Throwable> failure = new AtomicReference<>();

        Fixture(Response response) throws Exception {
            Context testContext = InstrumentationRegistry.getInstrumentation().getContext();
            try (java.io.InputStream stream = testContext.getAssets().open("local_tls_ca.pem")) {
                trustPem = stream.readAllBytes();
            }
            char[] password = "maichat-test".toCharArray();
            KeyStore store = KeyStore.getInstance("PKCS12");
            try (java.io.InputStream stream = testContext.getAssets().open("local_tls_server.p12")) {
                store.load(stream, password);
            }
            KeyManagerFactory manager = KeyManagerFactory.getInstance(KeyManagerFactory.getDefaultAlgorithm());
            manager.init(store, password);
            SSLContext tls = SSLContext.getInstance("TLS");
            tls.init(manager.getKeyManagers(), null, null);
            server = (SSLServerSocket) tls.getServerSocketFactory().createServerSocket(
                0, 1, InetAddress.getByName("localhost"));
            server.setSoTimeout(8000);
            worker = new Thread(() -> {
                try (SSLSocket socket = (SSLSocket) server.accept()) {
                    socket.setSoTimeout(8000);
                    BufferedReader input = new BufferedReader(new InputStreamReader(
                        socket.getInputStream(), StandardCharsets.US_ASCII));
                    String line;
                    while ((line = input.readLine()) != null && !line.isEmpty()) { }
                    response.write(socket.getOutputStream());
                } catch (Throwable error) {
                    failure.set(error);
                    android.util.Log.e("MaiChatTestTLS", "local TLS fixture failed", error);
                }
            }, "maichat-local-tls-download");
            worker.start();
        }

        String url() { return "https://localhost:" + server.getLocalPort() + "/attachment"; }

        @Override public void close() throws Exception {
            server.close();
            worker.join(9000);
            if (worker.isAlive()) throw new IOException("local TLS server did not finish");
            Throwable error = failure.get();
            if (error != null && !(error instanceof IOException))
                throw new IOException("local TLS server failed", error);
        }
    }
}
