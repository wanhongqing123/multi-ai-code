package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.media.MediaMetadataRetriever;
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
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.net.URL;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import org.json.JSONObject;
import org.junit.Assume;
import org.junit.Test;
import org.junit.runner.RunWith;

/** Explicit opt-in end-to-end canary for the dedicated production cloud bucket. */
@RunWith(AndroidJUnit4.class)
public final class MaiCloudDriveProductionCanaryTest {
    private static final String CREDENTIALS = "cloud-drive-canary-credentials.json";
    private static final String ENTRY = "打开我的云盘";

    @Test public void accountAUploadsPlaysAndDeletesWhileAccountBStaysClosed()
        throws Exception {
        Assume.assumeTrue("Run only with -e runCloudCanary 1",
            "1".equals(InstrumentationRegistry.getArguments().getString("runCloudCanary")));
        assertTrue("Production canary needs the isolated emulator package",
            BuildConfig.DEBUG && !BuildConfig.USE_LOCAL_UI_AUTH &&
            "com.kongshang.maichat.isolated".equals(BuildConfig.APPLICATION_ID) &&
            (Build.FINGERPRINT.contains("sdk_gphone") ||
             Build.FINGERPRINT.contains("generic")));

        Context context = ApplicationProvider.getApplicationContext();
        File credentials = new File(context.getFilesDir(), CREDENTIALS);
        assertTrue("Inject the private canary ledger before opting in", credentials.isFile());
        assertEquals("Canary credentials must be mode 0600", 0,
            Os.stat(credentials.getAbsolutePath()).st_mode & 0077);
        assertTrue("Canary credential file is unexpectedly large", credentials.length() < 4096);
        JSONObject accounts = new JSONObject(new String(Files.readAllBytes(credentials.toPath()),
            java.nio.charset.StandardCharsets.UTF_8)).getJSONObject("accounts");
        assertTrue("Delete the device credential file after reading", credentials.delete());
        JSONObject a = accounts.getJSONObject("A");
        JSONObject b = accounts.getJSONObject("B");
        String aId = a.getString("id"), bId = b.getString("id");
        assertTrue(aId.matches("cloudcanary_a_[0-9a-f]{12}"));
        assertTrue(bId.matches("cloudcanary_b_[0-9a-f]{12}"));
        assertFalse(aId.equals(bId));
        assertEquals("verified", a.getString("state"));
        assertEquals("verified", b.getString("state"));

        MaiChatAuthClient auth = MaiChatAuthClient.shared(context);
        File source = new File(context.getCacheDir(), "cloud-canary-" +
            java.util.UUID.randomUUID() + ".mp4");
        File downloaded = new File(context.getCacheDir(), "cloud-canary-download.mp4");
        File checkpoint = null;
        String videoId = null;
        boolean deleted = false;
        try {
            downloaded.delete();
            try (InputStream input = InstrumentationRegistry.getInstrumentation()
                     .getContext().getAssets().open("gallery-sample.mp4");
                 FileOutputStream output = new FileOutputStream(source)) {
                byte[] buffer = new byte[8192];
                int read;
                while ((read = input.read(buffer)) != -1) output.write(buffer, 0, read);
            }
            assertTrue(source.length() > 0);

            auth.discardLocalSession();
            auth.loginWithPassword(aId, a.getString("password"), true);
            assertEquals(aId, auth.accountId());
            assertEquals("https://ichat.life/api", auth.cloudDriveAccess().authBaseUrl);
            MaiCloudDrive client = new MaiCloudDrive(context);
            assertEquals(MaiCloudDrive.GATE_AVAILABLE, probe(context, aId));
            checkpoint = client.newCheckpoint();
            MaiCloudDrive.Result uploaded = upload(client, client.inspect(source), checkpoint);
            assertTrue("Production OSS upload failed; status=" + uploaded.status +
                ", http=" + uploaded.httpStatus, uploaded.ok());
            videoId = uploaded.json().optString("video_id", "");
            assertFalse("Completed upload retained checkpoint", checkpoint.exists());

            MaiCloudDrive.Result listing = request(client, MaiCloudDrive.LIST,
                new JSONObject());
            assertTrue("Account A list failed; http=" + listing.httpStatus, listing.ok());
            String name = source.getName();
            JSONObject video = findVideo(listing.json(), name);
            assertNotNull("Uploaded video missing from account A", video);
            videoId = video.getString("id");
            assertEquals(source.length(), video.getLong("size_bytes"));

            MaiCloudDrive.Result link = request(client, MaiCloudDrive.PLAY_LINK,
                new JSONObject().put("video_id", videoId));
            assertTrue("Private play link failed; http=" + link.httpStatus, link.ok());
            String url = link.json().getString("read_url");
            assertEquals("https", new URL(url).getProtocol());
            assertTrue("Read URL did not use dedicated private bucket",
                new URL(url).getHost().contains("maichat-private-cloud-drive-gz-20261025-f346b709"));
            MaiChatAttachmentDownload.Result download = download(url, downloaded);
            assertTrue("Private OSS download failed; http=" + download.httpStatus,
                download.ok());
            assertEquals(source.length(), downloaded.length());
            assertTrue("Downloaded bytes differ from uploaded source",
                MessageDigest.isEqual(sha256(source), sha256(downloaded)));
            MediaMetadataRetriever metadata = new MediaMetadataRetriever();
            try {
                metadata.setDataSource(downloaded.getAbsolutePath());
                Bitmap frame = metadata.getFrameAtTime(0);
                assertNotNull("Downloaded MP4 has no decodable frame", frame);
                frame.recycle();
            } finally { metadata.release(); }

            assertEntry(context, true);

            auth.discardLocalSession();
            auth.loginWithPassword(bId, b.getString("password"), true);
            assertEquals(bId, auth.accountId());
            assertEquals(MaiCloudDrive.GATE_CLOSED, probe(context, bId));
            assertEquals(404, request(new MaiCloudDrive(context), MaiCloudDrive.LIST,
                new JSONObject()).httpStatus);
            assertEquals(404, request(new MaiCloudDrive(context), MaiCloudDrive.PLAY_LINK,
                new JSONObject().put("video_id", videoId)).httpStatus);
            assertEntry(context, false);

            auth.discardLocalSession();
            auth.loginWithPassword(aId, a.getString("password"), true);
            assertEquals(aId, auth.accountId());
            MaiCloudDrive.Result removed = request(new MaiCloudDrive(context),
                MaiCloudDrive.DELETE, new JSONObject().put("video_id", videoId));
            assertTrue("Canary object cleanup failed; http=" + removed.httpStatus,
                removed.ok());
            deleted = true;
            JSONObject after = null;
            long cleanupDeadline = System.currentTimeMillis() + 30_000;
            do {
                MaiCloudDrive.Result postflight = request(new MaiCloudDrive(context),
                    MaiCloudDrive.LIST, new JSONObject());
                assertTrue("Account A post-delete list failed; http=" +
                    postflight.httpStatus, postflight.ok());
                after = postflight.json();
                JSONObject quota = after.getJSONObject("quota");
                if (after.getJSONArray("videos").length() == 0 &&
                    quota.getLong("used_bytes") == 0 &&
                    quota.getLong("reserved_bytes") == 0) break;
                Thread.sleep(200);
            } while (System.currentTimeMillis() < cleanupDeadline);
            assertNotNull(after);
            assertEquals("Account A still has cloud videos", 0,
                after.getJSONArray("videos").length());
            assertEquals("Account A still has used cloud bytes", 0,
                after.getJSONObject("quota").getLong("used_bytes"));
            assertEquals("Account A still reserves cloud bytes", 0,
                after.getJSONObject("quota").getLong("reserved_bytes"));
            assertEquals("Account A retained a local upload checkpoint", 0,
                checkpoints(new MaiCloudDrive(context)).getJSONArray("uploads").length());
        } finally {
            if (!deleted && videoId != null && !videoId.isEmpty()) {
                auth.discardLocalSession();
                auth.loginWithPassword(aId, a.getString("password"), true);
                MaiCloudDrive.Result cleanup = request(new MaiCloudDrive(context),
                    MaiCloudDrive.DELETE, new JSONObject().put("video_id", videoId));
                assertTrue("Canary video cleanup failed; inspect dedicated bucket",
                    cleanup.ok());
            }
            if (checkpoint != null && checkpoint.exists()) {
                auth.discardLocalSession();
                auth.loginWithPassword(aId, a.getString("password"), true);
                MaiCloudDrive.Result cleanup = abort(new MaiCloudDrive(context), checkpoint);
                assertTrue("Canary multipart cleanup failed; inspect dedicated bucket",
                    cleanup.ok());
            }
            auth.discardLocalSession();
            source.delete();
            downloaded.delete();
        }
    }

    private static int probe(Context context, String account) throws Exception {
        CountDownLatch done = new CountDownLatch(1);
        AtomicReference<Integer> result = new AtomicReference<>();
        MaiCloudDrive.probe(context, state -> { result.set(state); done.countDown(); });
        assertTrue("Cloud gate timed out", done.await(30, TimeUnit.SECONDS));
        assertEquals(account, MaiChatAuthClient.shared(context).accountId());
        return result.get();
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

    private static MaiCloudDrive.Result upload(MaiCloudDrive client,
                                               MaiCloudDrive.Source source,
                                               File checkpoint) throws Exception {
        CountDownLatch done = new CountDownLatch(1);
        AtomicReference<MaiCloudDrive.Result> result = new AtomicReference<>();
        client.upload(source, checkpoint, (bytes, total, preparing) -> { },
            value -> { result.set(value); done.countDown(); });
        assertTrue("Canary OSS upload timed out", done.await(120, TimeUnit.SECONDS));
        assertNotNull(result.get());
        return result.get();
    }

    private static MaiCloudDrive.Result abort(MaiCloudDrive client,
                                              File checkpoint) throws Exception {
        CountDownLatch done = new CountDownLatch(1);
        AtomicReference<MaiCloudDrive.Result> result = new AtomicReference<>();
        client.abort(checkpoint, value -> { result.set(value); done.countDown(); });
        assertTrue("Canary multipart abort timed out", done.await(45, TimeUnit.SECONDS));
        assertNotNull(result.get());
        return result.get();
    }

    private static JSONObject checkpoints(MaiCloudDrive client) throws Exception {
        CountDownLatch done = new CountDownLatch(1);
        AtomicReference<MaiCloudDrive.Result> result = new AtomicReference<>();
        client.checkpoints(value -> { result.set(value); done.countDown(); });
        assertTrue("Canary checkpoint list timed out", done.await(30, TimeUnit.SECONDS));
        assertNotNull(result.get());
        assertTrue("Canary checkpoint list failed", result.get().ok());
        return result.get().json();
    }

    private static MaiChatAttachmentDownload.Result download(String url, File destination)
        throws Exception {
        CountDownLatch done = new CountDownLatch(1);
        AtomicReference<MaiChatAttachmentDownload.Result> result = new AtomicReference<>();
        new MaiChatAttachmentDownload.NativeTransport().download(url, destination,
            MaiCloudDrive.MAX_VIDEO_BYTES, new MaiChatAttachmentDownload.Scope(),
            value -> { result.set(value); done.countDown(); });
        assertTrue("Private OSS download timed out", done.await(90, TimeUnit.SECONDS));
        assertNotNull(result.get());
        return result.get();
    }

    private static JSONObject findVideo(JSONObject listing, String name) throws Exception {
        org.json.JSONArray videos = listing.getJSONArray("videos");
        for (int i = 0; i < videos.length(); ++i) {
            JSONObject video = videos.getJSONObject(i);
            if (name.equals(video.optString("name"))) return video;
        }
        return null;
    }

    private static byte[] sha256(File file) throws Exception {
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        try (FileInputStream input = new FileInputStream(file)) {
            byte[] buffer = new byte[8192];
            int read;
            while ((read = input.read(buffer)) != -1) digest.update(buffer, 0, read);
        }
        return digest.digest();
    }

    private static void assertEntry(Context context, boolean expected) throws Exception {
        try (ActivityScenario<MainActivity> screen = ActivityScenario.launch(MainActivity.class)) {
            screen.onActivity(activity -> {
                View tab = byDescription(activity.getWindow().getDecorView(), "我");
                assertNotNull(tab);
                tab.performClick();
            });
            AtomicReference<View> entry = new AtomicReference<>();
            long deadline = System.currentTimeMillis() + 5000;
            do {
                screen.onActivity(activity -> entry.set(byDescription(
                    activity.getWindow().getDecorView(), ENTRY)));
                if (entry.get() != null && visible(entry.get()) == expected) break;
                Thread.sleep(100);
            } while (System.currentTimeMillis() < deadline);
            assertNotNull(entry.get());
            assertEquals("Personal page cloud entry visibility", expected,
                visible(entry.get()));
            if (expected) {
                screen.onActivity(activity -> entry.get().performClick());
                AccessibilityNodeInfo play = null;
                deadline = System.currentTimeMillis() + 15_000;
                do {
                    AccessibilityNodeInfo root = InstrumentationRegistry.getInstrumentation()
                        .getUiAutomation().getRootInActiveWindow();
                    if (root != null) play = textNode(root, "播放");
                    if (play != null) break;
                    Thread.sleep(100);
                } while (System.currentTimeMillis() < deadline);
                assertNotNull("Uploaded video did not appear in cloud UI", play);
                screenshot(context, "cloud-drive-canary-a-list.png");
                assertTrue(play.performAction(AccessibilityNodeInfo.ACTION_CLICK));
                deadline = System.currentTimeMillis() + 30_000;
                boolean playing = false;
                do {
                    MaiFfplayVideoView player = MaiFfplayVideoView.activeView();
                    playing = player != null && player.hasPresentedFrame() &&
                        player.currentVideoPath() != null &&
                        player.currentVideoPath().startsWith("maicloud-vod-");
                    if (playing) break;
                    Thread.sleep(100);
                } while (System.currentTimeMillis() < deadline);
                assertTrue("Cloud UI did not open video player", playing);
                screen.onActivity(activity ->
                    assertTrue(activity.commandAgentVideo("close", 0, 0, 0)));
            } else {
                screen.onActivity(activity -> {
                    View root = activity.getWindow().getDecorView();
                    View current = byDescription(root, ENTRY);
                    if (current == null) {
                        View tab = byDescription(root, "我");
                        assertNotNull(tab);
                        tab.performClick();
                        current = byDescription(root, ENTRY);
                    }
                    assertNotNull("Account B personal page lacks cloud row layout", current);
                    assertFalse("Account B cloud entry became visible", visible(current));
                    screenshotView(context, root, "cloud-drive-canary-b-hidden.png");
                });
            }
        }
    }

    private static void screenshotView(Context context, View root, String name) {
        assertTrue(root.getWidth() > 0 && root.getHeight() > 0);
        Bitmap image = Bitmap.createBitmap(root.getWidth(), root.getHeight(),
            Bitmap.Config.ARGB_8888);
        try {
            root.draw(new Canvas(image));
            File directory = context.getExternalFilesDir(null);
            assertNotNull(directory);
            try (FileOutputStream output = new FileOutputStream(new File(directory, name))) {
                assertTrue(image.compress(Bitmap.CompressFormat.PNG, 100, output));
            }
        } catch (Exception error) { throw new AssertionError("Canary UI snapshot failed", error); }
        finally { image.recycle(); }
    }

    private static void screenshot(Context context, String name) throws Exception {
        Bitmap image = InstrumentationRegistry.getInstrumentation().getUiAutomation()
            .takeScreenshot();
        assertNotNull("Canary UI screenshot unavailable", image);
        File directory = context.getExternalFilesDir(null);
        assertNotNull(directory);
        try (FileOutputStream output = new FileOutputStream(new File(directory, name))) {
            assertTrue(image.compress(Bitmap.CompressFormat.PNG, 100, output));
        } finally { image.recycle(); }
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

    private static boolean visible(View child) {
        View current = child;
        while (current != null) {
            if (current.getVisibility() != View.VISIBLE) return false;
            current = current.getParent() instanceof View ? (View) current.getParent() : null;
        }
        return true;
    }

    private static AccessibilityNodeInfo textNode(AccessibilityNodeInfo root,
                                                  String wanted) {
        if (root.getText() != null && wanted.contentEquals(root.getText())) return root;
        for (int i = 0; i < root.getChildCount(); ++i) {
            AccessibilityNodeInfo child = root.getChild(i);
            if (child == null) continue;
            AccessibilityNodeInfo found = textNode(child, wanted);
            if (found != null) return found;
        }
        return null;
    }
}
