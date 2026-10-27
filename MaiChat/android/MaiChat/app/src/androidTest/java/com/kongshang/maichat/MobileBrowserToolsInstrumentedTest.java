package com.kongshang.maichat;

import static org.junit.Assert.*;

import android.content.Context;
import android.content.Intent;
import android.graphics.Bitmap;
import androidx.test.core.app.ActivityScenario;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import com.kongshang.maichat.tools.MobileBrowserTools;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.Test;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
public final class MobileBrowserToolsInstrumentedTest {
    @Test public void visibleBrowserCanReadActAndCaptureInsideItsSession() throws Exception {
        String html = "<!doctype html><html><head><meta name='viewport' " +
            "content='width=device-width,initial-scale=1'><title>Fixture Browser</title></head>" +
            "<body style='background:#fff;color:#111;font-size:22px'>" +
            "<h1>Fixture Browser</h1><input placeholder='Note' oninput=\"" +
            "document.getElementById('result').textContent='typed:'+this.value\">" +
            "<input type='password' placeholder='Password'>" +
            "<button onclick=\"document.getElementById('result').textContent='clicked'\">Run</button>" +
            "<p id='result'>idle</p></body></html>";
        byte[] body = html.getBytes(StandardCharsets.UTF_8);
        try (ServerSocket server = new ServerSocket(0, 4,
                InetAddress.getByName("127.0.0.1"))) {
            Thread serverThread = new Thread(() -> {
                while (!server.isClosed()) {
                    try (Socket connection = server.accept()) {
                        InputStream input = connection.getInputStream();
                        int matched = 0;
                        while (matched < 4) {
                            int value = input.read();
                            if (value < 0) break;
                            matched = value == "\r\n\r\n".charAt(matched) ? matched + 1
                                : value == '\r' ? 1 : 0;
                        }
                        byte[] header = ("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n" +
                            "Content-Length: " + body.length + "\r\nConnection: close\r\n\r\n")
                            .getBytes(StandardCharsets.US_ASCII);
                        connection.getOutputStream().write(header);
                        connection.getOutputStream().write(body);
                    } catch (Exception failure) {
                        if (!server.isClosed()) throw new AssertionError(failure);
                    }
                }
            }, "browser-fixture");
            serverThread.start();
            Context app = InstrumentationRegistry.getInstrumentation().getTargetContext();
            Intent intent = new Intent(app, AIAssistantTestActivity.class);
            try (ActivityScenario<AIAssistantTestActivity> scenario = ActivityScenario.launch(intent)) {
                waitFor(scenario, a -> a.panel.controller.state.ready);
                CompletableFuture<Void> created = new CompletableFuture<>();
                scenario.onActivity(a -> a.panel.controller.action("create", new JSONObject(),
                    () -> created.complete(null)));
                created.get(10, TimeUnit.SECONDS);
                waitFor(scenario, a -> !a.panel.controller.state.selected.isEmpty());
                AtomicReference<MobileBrowserTools> browser = new AtomicReference<>();
                AtomicReference<String> session = new AtomicReference<>();
                scenario.onActivity(a -> {
                    browser.set(a.browser);
                    session.set(a.panel.controller.state.selected);
                });
                String url = "http://127.0.0.1:" + server.getLocalPort() + "/page";
                assertEquals("loading", browser.get().execute(new JSONObject()
                    .put("action", "open").put("session_id", session.get())
                    .put("url", url)).getString("state"));
                JSONObject first = awaitPage(browser.get(), session.get(), "Fixture Browser");
                assertEquals(url, first.getString("url"));
                assertTrue(first.getString("text").contains("idle"));
                assertTrue(first.getJSONArray("elements").length() >= 3);
                String note = element(first, "Note"), password = element(first, "Password");
                assertThrows(Exception.class, () -> browser.get().execute(new JSONObject()
                    .put("action", "type").put("session_id", session.get())
                    .put("expected_url", first.getString("url"))
                    .put("snapshot_id", first.getString("snapshot_id"))
                    .put("element_id", password).put("expected_label", "Password")
                    .put("text", "secret")));
                assertEquals("dispatched", browser.get().execute(new JSONObject()
                    .put("action", "type").put("session_id", session.get())
                    .put("expected_url", first.getString("url"))
                    .put("snapshot_id", first.getString("snapshot_id"))
                    .put("element_id", note).put("expected_label", "Note")
                    .put("text", "hello")).getString("state"));
                assertThrows(Exception.class, () -> browser.get().execute(new JSONObject()
                    .put("action", "observe").put("session_id", "ses_other")));
                JSONObject typed = awaitPage(browser.get(), session.get(), "typed:hello");
                String run = element(typed, "Run");
                assertEquals("dispatched", browser.get().execute(new JSONObject()
                    .put("action", "click").put("session_id", session.get())
                    .put("expected_url", typed.getString("url"))
                    .put("snapshot_id", typed.getString("snapshot_id"))
                    .put("element_id", run).put("expected_label", "Run"))
                    .getString("state"));
                JSONObject clicked = awaitPage(browser.get(), session.get(), "clicked");
                assertThrows(Exception.class, () -> browser.get().execute(new JSONObject()
                    .put("action", "click").put("session_id", session.get())
                    .put("expected_url", typed.getString("url"))
                    .put("snapshot_id", typed.getString("snapshot_id"))
                    .put("element_id", run).put("expected_label", "Run")));
                AtomicReference<File> image = new AtomicReference<>();
                scenario.onActivity(a -> {
                    File folder = new File(a.getNoBackupFilesDir(), "browser");
                    assertTrue(folder.isDirectory() || folder.mkdirs());
                    image.set(new File(folder, "integration.png"));
                });
                JSONObject captured = browser.get().execute(new JSONObject()
                    .put("action", "screenshot").put("session_id", session.get())
                    .put("expected_url", clicked.getString("url"))
                    .put("output_path", image.get().getAbsolutePath()));
                assertEquals("captured", captured.getString("state"));
                assertTrue(image.get().isFile() && image.get().length() > 100);
                scenario.onActivity(a -> {
                    try {
                        Bitmap screen = InstrumentationRegistry.getInstrumentation()
                            .getUiAutomation().takeScreenshot();
                        assertNotNull(screen);
                        File output = new File(a.getExternalFilesDir(null), "browser-ui-test.png");
                        try (FileOutputStream file = new FileOutputStream(output)) {
                            assertTrue(screen.compress(Bitmap.CompressFormat.PNG, 100, file));
                        }
                        screen.recycle();
                    } catch (Exception failure) { throw new AssertionError(failure); }
                });
                assertEquals("closed", browser.get().execute(new JSONObject()
                    .put("action", "close").put("session_id", session.get()))
                    .getString("state"));
            }
            server.close();
            serverThread.join(3000);
        }
    }

    private static String element(JSONObject snapshot, String label) throws Exception {
        JSONArray elements = snapshot.getJSONArray("elements");
        for (int index = 0; index < elements.length(); ++index) {
            JSONObject item = elements.getJSONObject(index);
            if (label.equals(item.optString("label"))) return item.getString("id");
        }
        throw new AssertionError("browser element missing: " + label);
    }

    private static JSONObject awaitPage(MobileBrowserTools browser, String session,
                                        String expected) throws Exception {
        long deadline = System.currentTimeMillis() + 10000;
        JSONObject last = null;
        while (System.currentTimeMillis() < deadline) {
            JSONObject page = browser.execute(new JSONObject()
                .put("action", "observe").put("session_id", session));
            last = page;
            if ("ready".equals(page.optString("state")) &&
                (page.optString("text").contains(expected) ||
                 page.optString("title").contains(expected))) return page;
            Thread.sleep(100);
        }
        throw new AssertionError("browser page did not show: " + expected +
            "; last=" + (last == null ? "none" : last.toString()));
    }

    private interface StateCheck { boolean ready(AIAssistantTestActivity activity); }

    private static void waitFor(ActivityScenario<AIAssistantTestActivity> scenario,
                                StateCheck check) throws Exception {
        long deadline = System.currentTimeMillis() + 10000;
        while (System.currentTimeMillis() < deadline) {
            AtomicReference<Boolean> ready = new AtomicReference<>(false);
            scenario.onActivity(activity -> ready.set(check.ready(activity)));
            if (ready.get()) return;
            Thread.sleep(100);
        }
        throw new AssertionError("Assistant test Activity did not become ready");
    }
}
