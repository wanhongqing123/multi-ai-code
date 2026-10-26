package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;

import android.app.Dialog;
import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.os.Build;
import android.system.Os;
import android.view.View;
import android.view.ViewGroup;
import android.view.accessibility.AccessibilityNodeInfo;
import androidx.test.core.app.ActivityScenario;
import androidx.test.core.app.ApplicationProvider;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.ByteArrayOutputStream;
import java.lang.reflect.Field;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;
import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.Assume;
import org.junit.Test;
import org.junit.runner.RunWith;

/** Explicit opt-in real OSS image and video UI canary on the normal emulator app. */
@RunWith(AndroidJUnit4.class)
public final class MaiCloudDriveImageProductionCanaryTest {
    @Test public void normalAppAccountAUploadsThumbnailsPreviewsAndDeletes() throws Exception {
        Assume.assumeTrue("Run only with -e runCloudImageCanary 1",
            "1".equals(InstrumentationRegistry.getArguments().getString("runCloudImageCanary")));
        assertTrue("Use the normal emulator App only", BuildConfig.DEBUG &&
            !BuildConfig.USE_LOCAL_UI_AUTH &&
            "com.kongshang.maichat".equals(BuildConfig.APPLICATION_ID) &&
            (Build.FINGERPRINT.contains("sdk_gphone") || Build.FINGERPRINT.contains("generic")));
        Context context = ApplicationProvider.getApplicationContext();
        File credentials = new File(context.getFilesDir(),
            "cloud-drive-canary-credentials.json");
        assertTrue("Inject the private disposable A ledger", credentials.isFile());
        assertEquals(0, Os.stat(credentials.getAbsolutePath()).st_mode & 0077);
        assertTrue(credentials.length() < 4096);
        JSONObject account = new JSONObject(new String(Files.readAllBytes(
            credentials.toPath()), StandardCharsets.UTF_8))
            .getJSONObject("accounts").getJSONObject("A");
        assertTrue("Disposable A account only",
            account.getString("id").matches("cloudcanary_a_[0-9a-f]{12}"));
        assertEquals("verified", account.getString("state"));
        assertTrue("Do not leave the password on the emulator", credentials.delete());

        MaiChatAuthClient auth = MaiChatAuthClient.shared(context);
        auth.loginWithPassword(account.getString("id"), account.getString("password"), true);
        assertEquals(account.getString("id"), auth.accountId());
        MaiCloudDrive client = new MaiCloudDrive(context);
        MaiCloudDrive.Result baseline = request(client, MaiCloudDrive.LIST,
            new JSONObject());
        assertTrue(baseline.body, baseline.ok());
        JSONObject oldQuota = baseline.json().getJSONObject("quota");
        int oldCount = baseline.json().getJSONArray("videos").length();
        long oldUsed = oldQuota.getLong("used_bytes");
        long oldReserved = oldQuota.getLong("reserved_bytes");

        File image = new File(context.getCacheDir(), "cloud-canary-image.png");
        writeSourcePng(image);
        File video = new File(context.getCacheDir(), "cloud-canary-video.mp4");
        try (InputStream input = InstrumentationRegistry.getInstrumentation().getContext()
                 .getAssets().open("video-circle-source.mp4");
             FileOutputStream output = new FileOutputStream(video)) {
            byte[] buffer = new byte[8192];
            int count;
            while ((count = input.read(buffer)) != -1) output.write(buffer, 0, count);
        }
        List<String> created = new ArrayList<>();
        List<File> checkpoints = new ArrayList<>();
        try {
            String imageId = upload(client, image, "image", checkpoints);
            created.add(imageId);
            String videoId = upload(client, video, "video", checkpoints);
            created.add(videoId);
            assertOriginal(client, context, imageId, image);

            MaiCloudDrive.Result listed = request(client, MaiCloudDrive.LIST,
                new JSONObject());
            assertTrue(listed.body, listed.ok());
            assertEquals(oldCount + 2, listed.json().getJSONArray("videos").length());
            assertThumbnail(client, context, imageId, "image");
            assertThumbnail(client, context, videoId, "video");

            try (ActivityScenario<MainActivity> screen =
                     ActivityScenario.launch(MainActivity.class)) {
                AtomicBoolean opened = new AtomicBoolean();
                long deadline = System.currentTimeMillis() + 15_000;
                while (System.currentTimeMillis() < deadline && !opened.get()) {
                    screen.onActivity(activity -> {
                        View tab = byDescription(activity.getWindow().getDecorView(), "云盘");
                        if (tab != null && tab.isShown()) {
                            tab.performClick();
                            opened.set(true);
                        }
                    });
                    if (!opened.get()) Thread.sleep(100);
                }
                assertTrue("Normal App navigation did not finish loading", opened.get());
                opened.set(false);
                deadline = System.currentTimeMillis() + 15_000;
                while (System.currentTimeMillis() < deadline && !opened.get()) {
                    screen.onActivity(activity -> {
                        View page = byDescription(activity.getWindow().getDecorView(),
                            "云盘状态");
                        if (page != null && page.isShown()) {
                            opened.set(true);
                        }
                    });
                    if (!opened.get()) Thread.sleep(100);
                }
                assertTrue("Normal App cloud page did not become visible", opened.get());
                File thumbnailDirectory = new File(context.getCacheDir(),
                    "cloud-drive-thumbnails/" + MaiCloudDrive.accountKey(auth.accountId()));
                deadline = System.currentTimeMillis() + 30_000;
                while (System.currentTimeMillis() < deadline &&
                    decodedJpegs(thumbnailDirectory) < 2) Thread.sleep(150);
                assertEquals("Both private UI thumbnails must be rendered", 2,
                    decodedJpegs(thumbnailDirectory));
                screenshot(context, "cloud-canary-normal-media-list.png");

                clickText("查看");
                File preview = waitForFile(new File(context.getCacheDir(),
                    "cloud-drive-images"), ".png", 20_000);
                assertNotNull("Image download did not finish", preview);
                assertNotNull(BitmapFactory.decodeFile(preview.getAbsolutePath()));
                assertTrue("Image viewer did not open",
                    waitForImageViewer(screen, 10_000));
                Thread.sleep(1500);
                screenshot(context, "cloud-canary-normal-image-preview.png");
                screen.onActivity(activity -> {
                    try {
                        Field field = MainActivity.class.getDeclaredField("imageDialog");
                        field.setAccessible(true);
                        ((Dialog) field.get(activity)).dismiss();
                    } catch (Exception failure) { throw new AssertionError(failure); }
                });
                deadline = System.currentTimeMillis() + 5000;
                while (preview.exists() && System.currentTimeMillis() < deadline)
                    Thread.sleep(100);
                assertFalse("Private image cache survived viewer close", preview.exists());

                clickText("播放");
                String playing = null;
                deadline = System.currentTimeMillis() + 30_000;
                while (System.currentTimeMillis() < deadline) {
                    MaiFfplayVideoView active = MaiFfplayVideoView.activeView();
                    if (active != null && active.hasPresentedFrame() &&
                        active.currentVideoPath() != null &&
                        active.currentVideoPath().startsWith("maicloud-vod-")) {
                        playing = active.currentVideoPath();
                        break;
                    }
                    Thread.sleep(100);
                }
                assertNotNull("Cloud video did not open", playing);
                assertFalse("Cloud VOD unexpectedly wrote a media file",
                    new File(playing).exists());
                screen.onActivity(activity -> {
                    assertTrue(activity.commandAgentVideo("close", 0, 0, 0));
                    activity.currentCloudDriveDialog().dismiss();
                });
            }
        } finally {
            for (String id : created) {
                MaiCloudDrive.Result removed = request(new MaiCloudDrive(context),
                    MaiCloudDrive.DELETE, new JSONObject().put("video_id", id));
                assertTrue("Canary cleanup failed for " + id, removed.ok());
            }
            for (File checkpoint : checkpoints) if (checkpoint.exists()) {
                CountDownLatch done = new CountDownLatch(1);
                AtomicReference<MaiCloudDrive.Result> aborted = new AtomicReference<>();
                new MaiCloudDrive(context).abort(checkpoint, value -> {
                    aborted.set(value); done.countDown();
                });
                assertTrue("Canary multipart abort timed out", done.await(45, TimeUnit.SECONDS));
                assertTrue("Canary multipart cleanup failed", aborted.get().ok());
            }
            image.delete();
            video.delete();
        }
        MaiCloudDrive.Result after = request(new MaiCloudDrive(context), MaiCloudDrive.LIST,
            new JSONObject());
        assertTrue(after.body, after.ok());
        assertEquals(oldCount, after.json().getJSONArray("videos").length());
        JSONObject quota = after.json().getJSONObject("quota");
        assertEquals(oldUsed, quota.getLong("used_bytes"));
        assertTrue("Canary added reserved bytes after deletion",
            quota.getLong("reserved_bytes") <= oldReserved);
    }

    private static void writeSourcePng(File image) throws Exception {
        Bitmap pixels = Bitmap.createBitmap(128, 80, Bitmap.Config.ARGB_8888);
        pixels.eraseColor(0xff1a92db);
        try (FileOutputStream output = new FileOutputStream(image)) {
            assertTrue(pixels.compress(Bitmap.CompressFormat.PNG, 100, output));
        } finally { pixels.recycle(); }
    }

    private static String upload(MaiCloudDrive client, File file, String kind,
                                 List<File> checkpoints) throws Exception {
        MaiCloudDrive.Source source = client.inspect(file);
        File checkpoint = client.newCheckpoint();
        checkpoints.add(checkpoint);
        CountDownLatch done = new CountDownLatch(1);
        AtomicReference<MaiCloudDrive.Result> result = new AtomicReference<>();
        client.upload(source, checkpoint, (bytes, total, preparing) -> { }, value -> {
            result.set(value); done.countDown();
        });
        assertTrue("Real OSS upload timed out", done.await(120, TimeUnit.SECONDS));
        assertNotNull(result.get());
        assertTrue(result.get().body, result.get().ok());
        assertEquals(kind, result.get().json().getString("media_type"));
        assertFalse(checkpoint.exists());
        return result.get().json().getString("video_id");
    }

    private static void assertThumbnail(MaiCloudDrive client, Context context, String id,
                                        String kind)
        throws Exception {
        MaiCloudDrive.Result link = request(client, MaiCloudDrive.THUMBNAIL_LINK,
            new JSONObject().put("video_id", id));
        assertTrue(link.body, link.ok());
        assertEquals("image/jpeg", link.json().getString("content_type"));
        File file = new File(context.getCacheDir(), "canary-thumb-" + id + ".jpg");
        CountDownLatch done = new CountDownLatch(1);
        AtomicReference<MaiChatAttachmentDownload.Result> result = new AtomicReference<>();
        String url = link.json().getString("thumbnail_url");
        String transform = android.net.Uri.parse(url).getQueryParameter("x-oss-process");
        assertNotNull(kind + " thumbnail transform missing", transform);
        assertTrue(kind + " thumbnail transform mismatch",
            transform.startsWith(kind.equals("image") ? "image/resize," : "video/snapshot,"));
        new MaiChatAttachmentDownload.NativeTransport().download(
            url, file, 2L * 1024 * 1024,
            new MaiChatAttachmentDownload.Scope(), value -> {
                result.set(value); done.countDown();
            });
        assertTrue("Real thumbnail download timed out", done.await(60, TimeUnit.SECONDS));
        assertNotNull(result.get());
        assertTrue("Real " + kind + " thumbnail download failed: " + result.get().httpStatus +
            ", " + (result.get().ok() ? "" : diagnoseOss(url)), result.get().ok());
        Bitmap bitmap = BitmapFactory.decodeFile(file.getAbsolutePath());
        assertNotNull("OSS thumbnail could not be decoded", bitmap);
        bitmap.recycle();
        file.delete();
    }

    private static void assertOriginal(MaiCloudDrive client, Context context, String id,
                                       File source) throws Exception {
        MaiCloudDrive.Result link = request(client, MaiCloudDrive.PLAY_LINK,
            new JSONObject().put("video_id", id));
        assertTrue(link.body, link.ok());
        File target = new File(context.getCacheDir(), "cloud-canary-original.png");
        CountDownLatch done = new CountDownLatch(1);
        AtomicReference<MaiChatAttachmentDownload.Result> result = new AtomicReference<>();
        new MaiChatAttachmentDownload.NativeTransport().download(
            link.json().getString("read_url"), target, 20_000_000L,
            new MaiChatAttachmentDownload.Scope(), value -> {
                result.set(value); done.countDown();
            });
        assertTrue("Original image download timed out", done.await(60, TimeUnit.SECONDS));
        assertNotNull(result.get());
        assertTrue("Original image download failed", result.get().ok());
        assertEquals(source.length(), target.length());
        byte[] original = Files.readAllBytes(source.toPath());
        byte[] downloaded = Files.readAllBytes(target.toPath());
        assertTrue("Original image SHA-256 differs after shared multipart upload",
            MessageDigest.isEqual(MessageDigest.getInstance("SHA-256").digest(original),
                MessageDigest.getInstance("SHA-256").digest(downloaded)));
        assertTrue("Original OSS image is missing PNG magic",
            downloaded.length >= 8 && downloaded[0] == (byte) 0x89 &&
                downloaded[1] == 0x50 && downloaded[2] == 0x4e &&
                downloaded[3] == 0x47 && downloaded[4] == 0x0d &&
                downloaded[5] == 0x0a && downloaded[6] == 0x1a &&
                downloaded[7] == 0x0a);
        Bitmap bitmap = BitmapFactory.decodeFile(target.getAbsolutePath());
        assertNotNull("Original OSS PNG could not be decoded on Android", bitmap);
        bitmap.recycle();
        target.delete();
    }

    private static String diagnoseOss(String url) {
        // Test-only independent probe. Never print the temporary signed URL,
        // canonical request, object key, or authorization material.
        HttpURLConnection connection = null;
        try {
            connection = (HttpURLConnection) new URL(url).openConnection();
            connection.setInstanceFollowRedirects(false);
            connection.setConnectTimeout(8000);
            connection.setReadTimeout(8000);
            int status = connection.getResponseCode();
            try (InputStream input = status >= 400 ? connection.getErrorStream() :
                     connection.getInputStream();
                 ByteArrayOutputStream output = new ByteArrayOutputStream()) {
                if (input != null) {
                    byte[] buffer = new byte[512];
                    int count;
                    while (output.size() < 4096 && (count = input.read(buffer)) > 0)
                        output.write(buffer, 0, Math.min(count, 4096 - output.size()));
                }
                String xml = output.toString(StandardCharsets.UTF_8.name());
                java.util.regex.Matcher match = java.util.regex.Pattern.compile(
                    "<Code>([^<]{1,80})</Code>").matcher(xml);
                java.util.regex.Matcher reason = java.util.regex.Pattern.compile(
                    "<Message>([^<]{1,200})</Message>").matcher(xml);
                String message = reason.find() ? reason.group(1) : "none";
                if (!message.matches("[A-Za-z0-9 .,:;()'_-]{1,160}")) message = "redacted";
                return "independentHttp=" + status + ", ossCode=" +
                    (match.find() ? match.group(1) : "none") + ", ossMessage=" + message;
            }
        } catch (Exception failure) {
            return "independentProbe=" + failure.getClass().getSimpleName();
        } finally { if (connection != null) connection.disconnect(); }
    }

    private static MaiCloudDrive.Result request(MaiCloudDrive client, int action,
                                                JSONObject payload) throws Exception {
        CountDownLatch done = new CountDownLatch(1);
        AtomicReference<MaiCloudDrive.Result> result = new AtomicReference<>();
        client.request(action, payload, value -> { result.set(value); done.countDown(); });
        assertTrue("Cloud request timed out", done.await(45, TimeUnit.SECONDS));
        assertNotNull(result.get());
        return result.get();
    }

    private static int decodedJpegs(File directory) {
        File[] files = directory.listFiles((unused, name) -> name.endsWith(".jpg"));
        int decoded = 0;
        if (files != null) for (File file : files) {
            Bitmap bitmap = BitmapFactory.decodeFile(file.getAbsolutePath());
            if (bitmap != null) { decoded++; bitmap.recycle(); }
        }
        return decoded;
    }

    private static File waitForFile(File directory, String suffix, long timeoutMs)
        throws InterruptedException {
        long deadline = System.currentTimeMillis() + timeoutMs;
        while (System.currentTimeMillis() < deadline) {
            File[] files = directory.listFiles((unused, name) -> name.endsWith(suffix));
            if (files != null && files.length > 0 && files[0].isFile()) return files[0];
            Thread.sleep(100);
        }
        return null;
    }

    private static boolean waitForImageViewer(ActivityScenario<MainActivity> screen,
                                              long timeoutMs) throws Exception {
        AtomicBoolean opened = new AtomicBoolean();
        long deadline = System.currentTimeMillis() + timeoutMs;
        while (System.currentTimeMillis() < deadline && !opened.get()) {
            screen.onActivity(activity -> {
                try {
                    Field field = MainActivity.class.getDeclaredField("imageDialog");
                    field.setAccessible(true);
                    Dialog dialog = (Dialog) field.get(activity);
                    opened.set(dialog != null && dialog.isShowing());
                } catch (Exception failure) { throw new AssertionError(failure); }
            });
            if (!opened.get()) Thread.sleep(100);
        }
        return opened.get();
    }

    private static View byDescription(View root, String description) {
        if (root.getContentDescription() != null &&
            description.contentEquals(root.getContentDescription())) return root;
        if (root instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) root;
            for (int i = 0; i < group.getChildCount(); ++i) {
                View found = byDescription(group.getChildAt(i), description);
                if (found != null) return found;
            }
        }
        return null;
    }

    private static void clickText(String text) {
        AccessibilityNodeInfo root = InstrumentationRegistry.getInstrumentation()
            .getUiAutomation().getRootInActiveWindow();
        assertNotNull(root);
        AccessibilityNodeInfo found = textNode(root, text);
        assertNotNull(text + " button", found);
        assertTrue(found.performAction(AccessibilityNodeInfo.ACTION_CLICK));
    }

    private static AccessibilityNodeInfo textNode(AccessibilityNodeInfo root, String text) {
        if (root.getText() != null && text.contentEquals(root.getText())) return root;
        for (int i = 0; i < root.getChildCount(); ++i) {
            AccessibilityNodeInfo child = root.getChild(i);
            if (child != null) {
                AccessibilityNodeInfo found = textNode(child, text);
                if (found != null) return found;
            }
        }
        return null;
    }

    private static void screenshot(Context context, String name) throws Exception {
        Bitmap image = InstrumentationRegistry.getInstrumentation().getUiAutomation()
            .takeScreenshot();
        assertNotNull(image);
        try (FileOutputStream output = new FileOutputStream(
                new File(context.getFilesDir(), name))) {
            assertTrue(image.compress(Bitmap.CompressFormat.PNG, 100, output));
        } finally { image.recycle(); }
    }
}
