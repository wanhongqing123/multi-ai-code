package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;

import android.content.Context;
import android.graphics.Bitmap;
import android.media.MediaMetadataRetriever;
import android.view.accessibility.AccessibilityNodeInfo;
import android.view.View;
import android.view.ViewGroup;
import androidx.test.core.app.ActivityScenario;
import androidx.test.core.app.ApplicationProvider;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.file.Files;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.Consumer;
import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.Test;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
public final class MaiVideoCircleInstrumentedTest {
    private static final String PASSWORD = "local sync UI test passphrase";
    private static final String TITLE = "模拟器 AI 视频测试";

    @Test public void localTlsPublishReviewBrowseAndWithdraw() throws Exception {
        assertTrue("Use only the isolated local fixture", BuildConfig.DEBUG &&
            BuildConfig.USE_LOCAL_UI_AUTH);
        Context context = ApplicationProvider.getApplicationContext();
        byte[] certificate = fixture("/test/cert");
        MaiVideoCircle.setLocalFixturePemForTest(certificate);
        MaiChatAuthClient auth = MaiChatAuthClient.shared(context);
        auth.loginWithPassword("sync-ui-test", PASSWORD, true);
        MaiVideoCircle circle = new MaiVideoCircle(context);
        JSONObject card = new JSONObject()
            .put("tool", "agent_send_media")
            .put("state", "completed")
            .put("output", new JSONObject()
                .put("delivery", "current_ai_session")
                .put("type", "video")
                .put("mime_type", "video/mp4")
                .put("path", "sample.mp4")
                .put("source_tool", "wan_video")
                .put("source_task_id", "fixture-task-1"));
        assertTrue(MaiVideoCircle.inspectCard(card).ok());
        JSONObject invalid = new JSONObject(card.toString());
        invalid.getJSONObject("output").remove("source_task_id");
        assertFalse(MaiVideoCircle.inspectCard(invalid).ok());
        assertEquals(0, request(circle, MaiVideoCircle.FEED, new JSONObject())
            .json().getJSONArray("posts").length());

        File source = new File(context.getCacheDir(), "video-circle-source.mp4");
        try (InputStream input = InstrumentationRegistry.getInstrumentation()
                 .getContext().getAssets().open("video-circle-source.mp4")) {
            Files.copy(input, source.toPath(), java.nio.file.StandardCopyOption.REPLACE_EXISTING);
        }
        try (ActivityScenario<MainActivity> screen = ActivityScenario.launch(MainActivity.class)) {
            screen.onActivity(activity ->
                MaiVideoCircleDialog.confirmPublish(activity, circle, card, source));
            screenshot(context, "video-circle-publish.png");
        }
        MaiVideoCircle.PublishResult submitted = publish(circle, card, source);
        assertTrue(submitted.message + " status=" +
            (submitted.result == null ? -1 : submitted.result.status), submitted.ok());
        assertEquals("pending", submitted.result.json().optString("status"));
        String postId = submitted.result.json().getString("post_id");
        assertFalse(postId.isEmpty());
        assertEquals("pending", request(circle, MaiVideoCircle.MINE, new JSONObject())
            .json().getJSONArray("posts").getJSONObject(0).getString("status"));
        assertEquals("Mine cursor was not sent through the shared client", 400,
            request(circle, MaiVideoCircle.MINE, value("before", "not_a_cursor")).httpStatus);

        auth.loginWithPassword("sync-ui-viewer", PASSWORD, true);
        assertEquals(0, request(circle, MaiVideoCircle.FEED, new JSONObject())
            .json().getJSONArray("posts").length());
        JSONObject pendingLink = value("post_id", postId);
        assertEquals(404, request(circle, MaiVideoCircle.PLAY_LINK, pendingLink).httpStatus);

        JSONObject approved = new JSONObject(new String(fixture(
            "/test/video-circle/approve?post_id=" + postId),
            java.nio.charset.StandardCharsets.UTF_8));
        assertEquals("approved", approved.optString("status"));
        JSONArray feed = request(circle, MaiVideoCircle.FEED, new JSONObject())
            .json().getJSONArray("posts");
        assertEquals(1, feed.length());
        assertEquals(postId, feed.getJSONObject(0).getString("id"));
        String authorId = feed.getJSONObject(0).getString("author_id");
        String readUrl = request(circle, MaiVideoCircle.PLAY_LINK, pendingLink)
            .json().getString("read_url");
        assertTrue(readUrl.startsWith("https://localhost:18444/"));
        File downloaded = new File(context.getCacheDir(), "video-circle-playback-test.mp4");
        MaiChatAttachmentDownload.Scope scope = new MaiChatAttachmentDownload.Scope();
        CountDownLatch downloadDone = new CountDownLatch(1);
        AtomicReference<MaiChatAttachmentDownload.Result> downloadedResult =
            new AtomicReference<>();
        new MaiChatAttachmentDownload.NativeTransport(() -> certificate).download(
            readUrl, downloaded, 200_000_000, scope, result -> {
                downloadedResult.set(result);
                downloadDone.countDown();
            });
        assertTrue(downloadDone.await(30, TimeUnit.SECONDS));
        assertNotNull(downloadedResult.get());
        assertTrue(downloadedResult.get().ok());
        assertTrue(downloaded.length() > 0);
        MediaMetadataRetriever metadata = new MediaMetadataRetriever();
        try {
            metadata.setDataSource(downloaded.getAbsolutePath());
            Bitmap frame = metadata.getFrameAtTime(0);
            assertNotNull("Downloaded AI-marked video is not decodable", frame);
            File image = new File(context.getExternalFilesDir(null),
                "video-circle-marked-frame.png");
            try (FileOutputStream output = new FileOutputStream(image)) {
                assertTrue(frame.compress(Bitmap.CompressFormat.PNG, 100, output));
            }
            frame.recycle();
        } finally { metadata.release(); }
        try (ActivityScenario<MainActivity> screen = ActivityScenario.launch(MainActivity.class)) {
            screen.onActivity(activity -> {
                View assistantTab = findByDescription(
                    activity.getWindow().getDecorView(), "创作中心");
                assertNotNull("Creation center tab", assistantTab);
                assistantTab.performClick();
            });
            screenshot(context, "video-circle-entry.png");
            screen.onActivity(activity -> {
                View circleEntry = findByDescription(
                    activity.getWindow().getDecorView(), "打开 AI 视频圈");
                assertNotNull("Video circle entry", circleEntry);
                circleEntry.performClick();
            });
            screenshot(context, "video-circle-discover.png");
        }

        assertTrue(request(circle, MaiVideoCircle.REPORT, pendingLink.put("reason", "spam")).ok());
        assertTrue(request(circle, MaiVideoCircle.BLOCK, value("author_id", authorId)).ok());
        assertEquals(0, request(circle, MaiVideoCircle.FEED, new JSONObject())
            .json().getJSONArray("posts").length());
        assertEquals(1, request(circle, MaiVideoCircle.BLOCKS_LIST, new JSONObject())
            .json().getJSONArray("authors").length());
        assertTrue(request(circle, MaiVideoCircle.UNBLOCK, value("author_id", authorId)).ok());
        assertEquals(1, request(circle, MaiVideoCircle.FEED, new JSONObject())
            .json().getJSONArray("posts").length());

        auth.loginWithPassword("sync-ui-test", PASSWORD, true);
        try (ActivityScenario<MainActivity> screen = ActivityScenario.launch(MainActivity.class)) {
            screen.onActivity(activity -> {
                View assistantTab = findByDescription(
                    activity.getWindow().getDecorView(), "创作中心");
                assertNotNull("Creation center tab", assistantTab);
                assistantTab.performClick();
            });
            Thread.sleep(900);
            screen.onActivity(activity -> {
                View circleEntry = findByDescription(
                    activity.getWindow().getDecorView(), "打开 AI 视频圈");
                assertNotNull("Video circle entry", circleEntry);
                circleEntry.performClick();
            });
            Thread.sleep(900);
            AccessibilityNodeInfo root = InstrumentationRegistry.getInstrumentation()
                .getUiAutomation().getRootInActiveWindow();
            assertNotNull(root);
            assertFalse("Own public post exposed report action", hasExactText(root, "举报"));
            assertFalse("Own public post exposed block action", hasExactText(root, "屏蔽作者"));
            screenshot(context, "video-circle-own-feed.png");
        }
        assertTrue(request(circle, MaiVideoCircle.WITHDRAW, value("post_id", postId)).ok());
        try (ActivityScenario<MainActivity> screen = ActivityScenario.launch(MainActivity.class)) {
            screen.onActivity(activity -> {
                View assistantTab = findByDescription(
                    activity.getWindow().getDecorView(), "创作中心");
                assertNotNull("Creation center tab", assistantTab);
                assistantTab.performClick();
            });
            Thread.sleep(900);
            screen.onActivity(activity -> {
                View circleEntry = findByDescription(
                    activity.getWindow().getDecorView(), "打开 AI 视频圈");
                assertNotNull("Video circle entry", circleEntry);
                circleEntry.performClick();
            });
            Thread.sleep(900);
            AccessibilityNodeInfo root = InstrumentationRegistry.getInstrumentation()
                .getUiAutomation().getRootInActiveWindow();
            assertNotNull("Video circle dialog accessibility tree", root);
            assertTrue("Mine tab is accessible", clickByDescription(root, "我的"));
            screenshot(context, "video-circle-mine.png");
        }
        auth.loginWithPassword("sync-ui-viewer", PASSWORD, true);
        assertEquals(0, request(circle, MaiVideoCircle.FEED, new JSONObject())
            .json().getJSONArray("posts").length());
        assertEquals(404, request(circle, MaiVideoCircle.PLAY_LINK,
            value("post_id", postId)).httpStatus);
        source.delete();
        downloaded.delete();
    }

    private static MaiVideoCircle.Result request(MaiVideoCircle client, int action,
                                                 JSONObject payload) throws Exception {
        CountDownLatch done = new CountDownLatch(1);
        AtomicReference<MaiVideoCircle.Result> result = new AtomicReference<>();
        client.request(action, payload, value -> {
            result.set(value);
            done.countDown();
        });
        assertTrue("Video circle request timed out", done.await(35, TimeUnit.SECONDS));
        assertNotNull(result.get());
        return result.get();
    }

    private static MaiVideoCircle.PublishResult publish(MaiVideoCircle client,
                                                        JSONObject card, File source)
        throws Exception {
        CountDownLatch done = new CountDownLatch(1);
        AtomicReference<MaiVideoCircle.PublishResult> result = new AtomicReference<>();
        client.publish(card, source, TITLE, value -> {
            result.set(value);
            done.countDown();
        });
        assertTrue("Video circle upload timed out", done.await(180, TimeUnit.SECONDS));
        assertNotNull(result.get());
        return result.get();
    }

    private static JSONObject value(String key, String value) throws Exception {
        return new JSONObject().put(key, value);
    }

    private static void screenshot(Context context, String name) throws Exception {
        Thread.sleep(1500);
        Bitmap image = InstrumentationRegistry.getInstrumentation()
            .getUiAutomation().takeScreenshot();
        assertNotNull("AI video circle UI screenshot", image);
        File file = new File(context.getExternalFilesDir(null), name);
        try (FileOutputStream output = new FileOutputStream(file)) {
            assertTrue(image.compress(Bitmap.CompressFormat.PNG, 100, output));
        }
        image.recycle();
    }

    private static View findByDescription(View root, String name) {
        if (root.getContentDescription() != null &&
            name.contentEquals(root.getContentDescription())) return root;
        if (root instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) root;
            for (int index = 0; index < group.getChildCount(); index++) {
                View found = findByDescription(group.getChildAt(index), name);
                if (found != null) return found;
            }
        }
        return null;
    }

    private static boolean clickByDescription(AccessibilityNodeInfo root, String name) {
        CharSequence description = root.getContentDescription();
        if (description != null && description.toString().startsWith(name) && root.isClickable())
            return root.performAction(AccessibilityNodeInfo.ACTION_CLICK);
        for (int index = 0; index < root.getChildCount(); index++) {
            AccessibilityNodeInfo child = root.getChild(index);
            if (child != null && clickByDescription(child, name)) return true;
        }
        return false;
    }

    private static boolean hasExactText(AccessibilityNodeInfo root, String value) {
        if (root.getText() != null && value.contentEquals(root.getText())) return true;
        for (int index = 0; index < root.getChildCount(); index++) {
            AccessibilityNodeInfo child = root.getChild(index);
            if (child != null && hasExactText(child, value)) return true;
        }
        return false;
    }

    private static byte[] fixture(String path) throws Exception {
        HttpURLConnection connection = (HttpURLConnection)
            new URL("http://10.0.2.2:18190" + path).openConnection();
        connection.setConnectTimeout(5000);
        connection.setReadTimeout(5000);
        try (InputStream input = connection.getInputStream();
             ByteArrayOutputStream output = new ByteArrayOutputStream()) {
            byte[] buffer = new byte[4096];
            int count;
            while ((count = input.read(buffer)) != -1) output.write(buffer, 0, count);
            return output.toByteArray();
        } finally { connection.disconnect(); }
    }
}
