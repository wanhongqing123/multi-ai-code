// 文件作用：在 Android App 私有工作区验证下载与通用文件 API 的原子无覆盖发布。
// HTTP 仅绑定设备本机回环地址，不访问付费模型或公网素材。
package com.kongshang.maichat;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import java.io.BufferedReader;
import java.io.File;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.UUID;
import org.json.JSONObject;
import org.junit.Test;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
public final class DownloadPublishInstrumentedTest {
    static { System.loadLibrary("maichat_agent"); }

    private static native byte[] nativeDownload(byte[] arguments, byte[] workspace);
    private static native boolean nativePublishNewFile(byte[] source, byte[] destination);

    @Test public void localDownloadPublishesInAppPrivateWorkspaceWithoutOverwrite() throws Exception {
        File workspace = privateWorkspace();
        File output = new File(workspace, "clip.bin");
        byte[] expected = new byte[] {0, 1, 2, 3, (byte) 0xFF};
        try (LocalFixture fixture = new LocalFixture(expected, 2)) {
            JSONObject request = new JSONObject().put("url", fixture.url())
                .put("output_path", "clip.bin");
            JSONObject first = download(request, workspace);
            assertTrue(first.toString(), first.getBoolean("ok"));
            assertEquals(output.getCanonicalPath(),
                new File(new JSONObject(first.getString("output")).getString("path"))
                    .getCanonicalPath());
            assertArrayEquals(expected, Files.readAllBytes(output.toPath()));

            JSONObject duplicate = download(request, workspace);
            assertFalse(duplicate.toString(), duplicate.getBoolean("ok"));
            assertEquals("file_exists",
                new JSONObject(duplicate.getString("error")).getString("code"));
            assertArrayEquals("existing target must not be overwritten", expected,
                Files.readAllBytes(output.toPath()));
        } finally {
            Files.deleteIfExists(output.toPath());
            Files.deleteIfExists(workspace.toPath());
        }
    }

    @Test public void commonPublishMovesNewFileAndPreservesExistingTarget() throws Exception {
        File workspace = privateWorkspace();
        File source = new File(workspace, "source.bin");
        File target = new File(workspace, "target.bin");
        byte[] original = new byte[] {7, 8, 9};
        try {
            Files.write(source.toPath(), original);
            assertTrue(nativePublishNewFile(bytes(source.getAbsolutePath()),
                bytes(target.getAbsolutePath())));
            assertFalse(source.exists());
            assertArrayEquals(original, Files.readAllBytes(target.toPath()));

            Files.write(source.toPath(), new byte[] {4, 5, 6});
            assertFalse(nativePublishNewFile(bytes(source.getAbsolutePath()),
                bytes(target.getAbsolutePath())));
            assertArrayEquals(original, Files.readAllBytes(target.toPath()));
            assertArrayEquals(new byte[] {4, 5, 6}, Files.readAllBytes(source.toPath()));
        } finally {
            Files.deleteIfExists(source.toPath());
            Files.deleteIfExists(target.toPath());
            Files.deleteIfExists(workspace.toPath());
        }
    }

    private static JSONObject download(JSONObject request, File workspace) throws Exception {
        byte[] response = nativeDownload(bytes(request.toString()), bytes(workspace.getAbsolutePath()));
        assertTrue("native tool returned no response", response != null && response.length > 0);
        String text = new String(response, StandardCharsets.UTF_8);
        if (text.startsWith("OK\n"))
            return new JSONObject().put("ok", true).put("output", text.substring(3));
        if (text.startsWith("ERR\n"))
            return new JSONObject().put("ok", false).put("error", text.substring(4));
        throw new IllegalStateException("native tool returned an invalid response");
    }

    private static File privateWorkspace() {
        File root = new File(InstrumentationRegistry.getInstrumentation().getTargetContext().getFilesDir(),
            "DownloadPublishTest-" + UUID.randomUUID());
        assertTrue(root.mkdirs());
        return root;
    }

    private static byte[] bytes(String value) { return value.getBytes(StandardCharsets.UTF_8); }

    private static final class LocalFixture implements AutoCloseable {
        private final ServerSocket server;
        private final Thread worker;

        LocalFixture(byte[] body, int requests) throws IOException {
            server = new ServerSocket(0, requests, InetAddress.getByName("127.0.0.1"));
            server.setSoTimeout(10_000);
            worker = new Thread(() -> {
                for (int index = 0; index < requests; index++) {
                    try (Socket socket = server.accept()) {
                        socket.setSoTimeout(10_000);
                        BufferedReader input = new BufferedReader(new InputStreamReader(
                            socket.getInputStream(), StandardCharsets.US_ASCII));
                        input.readLine();
                        String header;
                        while ((header = input.readLine()) != null && !header.isEmpty()) { }
                        OutputStream output = socket.getOutputStream();
                        output.write(("HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
                            + "Content-Length: " + body.length + "\r\nConnection: close\r\n\r\n")
                            .getBytes(StandardCharsets.US_ASCII));
                        output.write(body);
                        output.flush();
                    } catch (IOException stopped) { return; }
                }
            }, "MaiChat-download-fixture");
            worker.start();
        }

        String url() { return "http://127.0.0.1:" + server.getLocalPort() + "/binary"; }

        @Override public void close() throws Exception {
            server.close();
            worker.join(10_000);
            assertFalse("local fixture thread is still running", worker.isAlive());
        }
    }
}
