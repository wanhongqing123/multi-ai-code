package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import android.content.ContentValues;
import android.content.ContentUris;
import android.content.Context;
import android.database.Cursor;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.media.MediaMetadataRetriever;
import android.net.Uri;
import android.provider.MediaStore;
import android.view.View;
import android.view.ViewGroup;
import android.view.accessibility.AccessibilityNodeInfo;
import androidx.test.core.app.ActivityScenario;
import androidx.test.core.app.ApplicationProvider;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.Map;
import java.util.HashSet;
import java.util.Set;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;
import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.Test;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
public final class MaiCloudDriveInstrumentedTest {
    private static final String PASSWORD = "local sync UI test passphrase";
    private static final String PREFS = "maichat-cloud-drive-emulator-test";
    private static final String URI_KEY = "source_uri";
    private static final String PIPE_RESUME_ID = "52b8df48-69db-4d04-a323-25550781b142";
    private static final long VIDEO_BYTES = 8L * 1024 * 1024 + 128L * 1024;

    @Test public void createOrphanAndPausedStageForColdStartCheck() throws Exception {
        assertTrue(BuildConfig.DEBUG && BuildConfig.USE_LOCAL_UI_AUTH &&
            BuildConfig.APPLICATION_ID.endsWith(".isolated"));
        Context context = ApplicationProvider.getApplicationContext();
        MaiChatAuthClient.shared(context).loginWithPassword("sync-ui-test", PASSWORD, true);
        MaiCloudDrive client = new MaiCloudDrive(context);
        File directory = client.accountDirectory();
        File orphan = new File(directory, "cold-orphan.maicloud.json.source");
        File paused = new File(directory, "cold-paused.maicloud.json.source");
        File checkpoint = new File(directory, "cold-paused.maicloud.json");
        orphan.delete(); paused.delete(); checkpoint.delete();
        try (FileOutputStream output = new FileOutputStream(orphan)) { output.write(7); }
        try (FileOutputStream output = new FileOutputStream(paused)) { output.write(8); }
        try (FileOutputStream output = new FileOutputStream(checkpoint)) { output.write(9); }
        client.accountDirectory();
        assertTrue("Live process must not sweep a stage before checkpoint creation",
            orphan.exists());
        assertTrue(paused.exists());
    }

    @Test public void coldStartSweepsOnlyUncheckpointedPrivateStages() throws Exception {
        assertTrue(BuildConfig.DEBUG && BuildConfig.USE_LOCAL_UI_AUTH &&
            BuildConfig.APPLICATION_ID.endsWith(".isolated"));
        Context context = ApplicationProvider.getApplicationContext();
        MaiChatAuthClient.shared(context).loginWithPassword("sync-ui-test", PASSWORD, true);
        File directory = new MaiCloudDrive(context).accountDirectory();
        File orphan = new File(directory, "cold-orphan.maicloud.json.source");
        File paused = new File(directory, "cold-paused.maicloud.json.source");
        File checkpoint = new File(directory, "cold-paused.maicloud.json");
        assertFalse("Restart left an orphaned private 2 GB-capable copy", orphan.exists());
        assertTrue("Paused upload source must survive restart", paused.exists());
        assertTrue(checkpoint.exists());
        paused.delete(); checkpoint.delete();
    }

    @Test public void imagePreviewRequiresEnoughSpaceAndReserve() {
        long bytes = 20_000_000L;
        long reserve = 64L * 1024 * 1024;
        assertFalse(MaiCloudDrivePlaybackCache.hasSpace(bytes + reserve - 1, bytes));
        assertTrue(MaiCloudDrivePlaybackCache.hasSpace(bytes + reserve, bytes));
    }

    @Test public void imageUploadShowsPrivateThumbnailAndFullScreenPreview() throws Exception {
        requireFixture();
        Context context = ApplicationProvider.getApplicationContext();
        MaiChatAuthClient.shared(context).loginWithPassword("sync-ui-test", PASSWORD, true);
        resetFixture();
        MaiCloudDrive client = new MaiCloudDrive(context);
        File image = new File(context.getCacheDir(), "cloud-test-image.png");
        Bitmap pixels = Bitmap.createBitmap(96, 64, Bitmap.Config.ARGB_8888);
        pixels.eraseColor(0xff21a7df);
        try (FileOutputStream output = new FileOutputStream(image)) {
            assertTrue(pixels.compress(Bitmap.CompressFormat.PNG, 100, output));
        } finally { pixels.recycle(); }
        MaiCloudDrive.Source source = client.inspect(image);
        assertEquals("image/png", source.type);
        assertEquals("image", MaiCloudDrive.mediaInfo(source.name).mediaType);
        assertEquals(20_000_000L, MaiCloudDrive.mediaInfo(source.name).maxBytes);
        assertEquals(null, MaiCloudDrive.mediaInfo("unsupported.gif"));

        File checkpoint = client.newCheckpoint();
        String id = null;
        try {
            MaiCloudDrive.Result uploaded = upload(client, source, checkpoint);
            assertTrue("Local cloud upload failed: status=" + uploaded.status +
                " http=" + uploaded.httpStatus + " curl=" + uploaded.curlCode +
                " body=" + uploaded.body, uploaded.ok());
            assertEquals("image", uploaded.json().getString("media_type"));
            MaiCloudDrive.Result listed = request(client, MaiCloudDrive.LIST, new JSONObject());
            assertTrue(listed.body, listed.ok());
            JSONObject item = listed.json().getJSONArray("videos").getJSONObject(0);
            id = item.getString("id");
            assertEquals("image", item.getString("media_type"));
            assertEquals(image.length(), item.getLong("size_bytes"));
            MaiCloudDrive.Result thumbnail = request(client, MaiCloudDrive.THUMBNAIL_LINK,
                new JSONObject().put("video_id", id));
            assertTrue(thumbnail.body, thumbnail.ok());
            assertEquals("image/jpeg", thumbnail.json().getString("content_type"));

            try (ActivityScenario<MainActivity> screen = ActivityScenario.launch(MainActivity.class)) {
                screen.onActivity(activity -> {
                    View tab = byDescription(activity.getWindow().getDecorView(), "云盘");
                    assertNotNull(tab);
                    tab.performClick();
                });
                Thread.sleep(300);
                File thumbnailDirectory = new File(context.getCacheDir(),
                    "cloud-drive-thumbnails/" +
                        MaiCloudDrive.accountKey(MaiChatAuthClient.shared(context).accountId()));
                File thumbnailFile = null;
                long deadline = System.currentTimeMillis() + 15_000;
                while (System.currentTimeMillis() < deadline) {
                    File[] files = thumbnailDirectory.listFiles();
                    if (files != null && files.length > 0) {
                        thumbnailFile = files[0];
                        if (BitmapFactory.decodeFile(thumbnailFile.getAbsolutePath()) != null) break;
                    }
                    Thread.sleep(100);
                }
                assertNotNull("Private thumbnail was not downloaded", thumbnailFile);
                assertNotNull("Private thumbnail was not decoded",
                    BitmapFactory.decodeFile(thumbnailFile.getAbsolutePath()));
                screenshot(context, "cloud-drive-image-thumbnail.png");
                AccessibilityNodeInfo window = InstrumentationRegistry.getInstrumentation()
                    .getUiAutomation().getRootInActiveWindow();
                assertNotNull(window);
                AccessibilityNodeInfo view = textNode(window, "查看");
                assertNotNull("Image preview button", view);
                assertTrue(view.performAction(AccessibilityNodeInfo.ACTION_CLICK));
                deadline = System.currentTimeMillis() + 15_000;
                File preview = null;
                while (System.currentTimeMillis() < deadline) {
                    File[] files = new File(context.getCacheDir(), "cloud-drive-images").listFiles();
                    if (files != null) for (File file : files) {
                        // The shared libcurl downloader publishes the final path atomically.
                        // Its private in-flight file is never a decodable preview.
                        if (file.isFile() && file.getName().endsWith(".png")) {
                            preview = file;
                            break;
                        }
                    }
                    if (preview != null) break;
                    Thread.sleep(100);
                }
                assertNotNull("Private image was not downloaded", preview);
                assertNotNull("Private full image could not be decoded",
                    BitmapFactory.decodeFile(preview.getAbsolutePath()));
                AtomicBoolean viewerOpened = new AtomicBoolean();
                deadline = System.currentTimeMillis() + 10_000;
                while (System.currentTimeMillis() < deadline && !viewerOpened.get()) {
                    screen.onActivity(activity -> {
                        try {
                            Field field = MainActivity.class.getDeclaredField("imageDialog");
                            field.setAccessible(true);
                            android.app.Dialog opened = (android.app.Dialog) field.get(activity);
                            viewerOpened.set(opened != null && opened.isShowing());
                        } catch (Exception failure) { throw new AssertionError(failure); }
                    });
                    if (!viewerOpened.get()) Thread.sleep(100);
                }
                assertTrue("Full-screen image viewer did not open", viewerOpened.get());
                Thread.sleep(1800);
                screenshot(context, "cloud-drive-image-preview.png");
                screen.onActivity(activity -> {
                    try {
                        Field field = MainActivity.class.getDeclaredField("imageDialog");
                        field.setAccessible(true);
                        android.app.Dialog previewDialog = (android.app.Dialog) field.get(activity);
                        assertNotNull("Full-screen image viewer did not open", previewDialog);
                        previewDialog.dismiss();
                        activity.currentCloudDriveDialog().dismiss();
                    } catch (Exception failure) { throw new AssertionError(failure); }
                });
                deadline = System.currentTimeMillis() + 5_000;
                while (preview.exists() && System.currentTimeMillis() < deadline)
                    Thread.sleep(50);
                assertFalse("Private preview remained after closing viewer", preview.exists());
                assertFalse("Private thumbnail remained after closing cloud page",
                    thumbnailFile.exists());
            }
        } finally {
            if (id != null) assertTrue(request(client, MaiCloudDrive.DELETE,
                new JSONObject().put("video_id", id)).ok());
            image.delete();
            checkpoint.delete();
        }
    }

    @Test public void accountSwitchClosesVodWithoutDeletingSource() throws Exception {
        requireFixture();
        assertTrue(BuildConfig.DEBUG && BuildConfig.USE_LOCAL_UI_AUTH &&
            BuildConfig.APPLICATION_ID.endsWith(".isolated"));
        Context context = ApplicationProvider.getApplicationContext();
        MaiChatAuthClient.shared(context).loginWithPassword("sync-ui-test", PASSWORD, true);
        resetFixture();
        File target = new File(context.getCacheDir(), "account-switch-source.mp4");
        try (InputStream input = InstrumentationRegistry.getInstrumentation()
                 .getContext().getAssets().open("gallery-sample.mp4");
             FileOutputStream output = new FileOutputStream(target)) {
            byte[] buffer = new byte[8192];
            int count;
            while ((count = input.read(buffer)) >= 0) output.write(buffer, 0, count);
        }
        MaiCloudDrive client = new MaiCloudDrive(context);
        MaiCloudDrive.Result uploaded = upload(client, client.inspect(target),
                                               client.newCheckpoint());
        assertTrue(uploaded.body, uploaded.ok());
        String id = request(client, MaiCloudDrive.LIST, new JSONObject())
            .json().getJSONArray("videos").getJSONObject(0).getString("id");
        MaiCloudVod.Opening opening = new MaiCloudVod.Opening();
        MaiCloudVod.Session source = client.openVodBlocking(id, "mp4", opening);
        try (ActivityScenario<MainActivity> screen = ActivityScenario.launch(MainActivity.class)) {
            screen.onActivity(activity -> assertTrue(
                activity.openCloudDriveVideo(source)));
            assertTrue("Point-of-demand session should be active", source.handle() != 0);
            assertFalse("Point-of-demand playback must not create a file",
                        new File(source.displayPath).exists());
            screen.onActivity(activity -> {
                try {
                    Field sessionField = MainActivity.class.getDeclaredField("session");
                    sessionField.setAccessible(true);
                    ((RemoteIMSessionController) sessionField.get(activity)).logout();
                    assertEquals("", MaiChatAuthClient.shared(activity).accountId());
                    Method render = MainActivity.class.getDeclaredMethod("render");
                    render.setAccessible(true);
                    render.invoke(activity);
                } catch (Exception error) { throw new AssertionError(error); }
            });
            long deadline = System.currentTimeMillis() + 5000;
            while (source.handle() != 0 && System.currentTimeMillis() < deadline)
                Thread.sleep(50);
            assertEquals("Account switch left VOD source active", 0, source.handle());
            assertTrue("Account switch removed original source", target.isFile());
        }
        target.delete();
    }

    @Test public void pauseUploadWithSeekableContentUri() throws Exception {
        requireFixture();
        resetFixture();
        Context context = ApplicationProvider.getApplicationContext();
        MaiChatAuthClient.shared(context).loginWithPassword("sync-ui-test", PASSWORD, true);
        MaiCloudDrive client = new MaiCloudDrive(context);
        File checkpoint = new File(client.accountDirectory(), "android-cloud-resume.maicloud.json");
        checkpoint.delete();
        Uri stale = Uri.parse(context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .getString(URI_KEY, ""));
        if ("content".equals(stale.getScheme()))
            try { context.getContentResolver().delete(stale, null, null); }
            catch (Exception ignored) { }
        Uri source = makeVideo(context);
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
            .putString(URI_KEY, source.toString()).commit();
        MaiCloudDrive.Source selected = client.inspect(source);
        assertEquals(VIDEO_BYTES, selected.bytes);
        assertTrue(selected.id().startsWith("content://"));
        AtomicBoolean paused = new AtomicBoolean();
        client.upload(selected, checkpoint, (done, total, staging) -> {
            if (!staging && done >= 8L * 1024 * 1024 && done < total &&
                paused.compareAndSet(false, true)) client.cancel();
        }, result -> { });
        long deadline = System.currentTimeMillis() + 90_000;
        while (!paused.get() && System.currentTimeMillis() < deadline)
            Thread.sleep(100);
        assertTrue("First part was not observed before cancellation", paused.get());
        JSONObject checkpoints = checkpoints(client);
        JSONArray uploads = checkpoints.getJSONArray("uploads");
        assertEquals(1, uploads.length());
        assertEquals(source.toString(), uploads.getJSONObject(0).getString("source_id"));
        assertEquals(checkpoint.getAbsolutePath(),
            uploads.getJSONObject(0).getString("checkpoint_path"));
        assertEquals(1, state().getInt("sessions"));
        assertEquals(0, state().getInt("oss_authorization"));
    }

    @Test public void resumeAfterRelaunchAndVerifyPrivateListPlayDelete() throws Exception {
        requireFixture();
        Context context = ApplicationProvider.getApplicationContext();
        MaiChatAuthClient auth = MaiChatAuthClient.shared(context);
        auth.loginWithPassword("sync-ui-test", PASSWORD, true);
        MaiCloudDrive client = new MaiCloudDrive(context);
        Uri sourceUri = Uri.parse(context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .getString(URI_KEY, ""));
        assertEquals("content", sourceUri.getScheme());
        File checkpoint = new File(client.accountDirectory(), "android-cloud-resume.maicloud.json");
        assertTrue("Previous-process checkpoint missing", checkpoint.isFile());
        MaiCloudDrive.Source source = client.inspect(sourceUri);
        MaiCloudDrive.Result completed = upload(client, source, checkpoint);
        assertTrue("Resume failed status=" + completed.status + " http=" + completed.httpStatus +
            " body=" + completed.body, completed.ok());
        assertFalse(checkpoint.exists());
        assertEquals(1, state().getInt("sessions"));
        assertEquals("ready", state().getString("status"));
        MaiCloudDrive.Result ownerList = request(client, MaiCloudDrive.LIST, new JSONObject());
        assertTrue(ownerList.body, ownerList.ok());
        JSONArray videos = ownerList.json().getJSONArray("videos");
        assertEquals(1, videos.length());
        String videoId = videos.getJSONObject(0).getString("id");
        assertEquals(VIDEO_BYTES, videos.getJSONObject(0).getLong("size_bytes"));
        assertTrue(ownerList.json().getJSONObject("quota").getLong("used_bytes") > 0);

        auth.loginWithPassword("sync-ui-viewer", PASSWORD, true);
        MaiCloudDrive viewer = new MaiCloudDrive(context);
        assertEquals(0, request(viewer, MaiCloudDrive.LIST, new JSONObject())
            .json().getJSONArray("videos").length());
        assertEquals(404, request(viewer, MaiCloudDrive.PLAY_LINK,
            new JSONObject().put("video_id", videoId)).httpStatus);

        auth.loginWithPassword("sync-ui-test", PASSWORD, true);
        MaiCloudDrive.Result link = request(client, MaiCloudDrive.PLAY_LINK,
            new JSONObject().put("video_id", videoId));
        assertTrue(link.body, link.ok());
        String url = link.json().getString("read_url");
        assertTrue(url.startsWith("https://localhost:18446/"));
        File downloaded = new File(context.getCacheDir(), "cloud-drive-playback-test.mp4");
        CountDownLatch downloadedDone = new CountDownLatch(1);
        AtomicReference<MaiChatAttachmentDownload.Result> downloadedResult = new AtomicReference<>();
        new MaiChatAttachmentDownload.NativeTransport(MaiCloudDriveInstrumentedTest::cert)
            .download(url, downloaded, MaiCloudDrive.MAX_VIDEO_BYTES,
                new MaiChatAttachmentDownload.Scope(), result -> {
                    downloadedResult.set(result); downloadedDone.countDown();
                });
        assertTrue("Private OSS download timed out", downloadedDone.await(60, TimeUnit.SECONDS));
        assertNotNull(downloadedResult.get());
        assertTrue(downloadedResult.get().ok());
        assertEquals(VIDEO_BYTES, downloaded.length());
        MediaMetadataRetriever metadata = new MediaMetadataRetriever();
        try {
            metadata.setDataSource(downloaded.getAbsolutePath());
            Bitmap first = metadata.getFrameAtTime(0);
            assertNotNull("Downloaded video could not be decoded", first);
            first.recycle();
        } finally { metadata.release(); }

        try (ActivityScenario<MainActivity> screen = ActivityScenario.launch(MainActivity.class)) {
            screen.onActivity(activity -> {
                View tab = byDescription(activity.getWindow().getDecorView(), "云盘");
                assertNotNull(tab);
                tab.performClick();
            });
            Thread.sleep(1200);
            screenshot(context, "cloud-drive-ready.png");
            AccessibilityNodeInfo root = InstrumentationRegistry.getInstrumentation()
                .getUiAutomation().getRootInActiveWindow();
            assertNotNull(root);
            AccessibilityNodeInfo play = textNode(root, "播放");
            assertNotNull("Cloud video play button", play);
            assertTrue(play.performAction(AccessibilityNodeInfo.ACTION_CLICK));
            String playbackSource = null;
            long deadline = System.currentTimeMillis() + 20_000;
            while (System.currentTimeMillis() < deadline) {
                MaiFfplayVideoView active = MaiFfplayVideoView.activeView();
                if (active != null && active.hasPresentedFrame() &&
                    active.currentVideoPath() != null &&
                    active.currentVideoPath().startsWith("maicloud-vod-")) {
                    playbackSource = active.currentVideoPath();
                    break;
                }
                Thread.sleep(100);
            }
            assertNotNull("Cloud VOD did not open", playbackSource);
            assertFalse("Cloud VOD unexpectedly wrote a media file",
                new File(playbackSource).exists());
            assertTrue("Cloud VOD did not use Range", state().getInt("range_requests") > 0);
            Thread.sleep(400); // Let the presented frame reach the Android view hierarchy.
            screenshot(context, "cloud-drive-vod-playing.png");
            long restartStarted = android.os.SystemClock.elapsedRealtime();
            screen.onActivity(activity -> {
                MaiFfplayVideoView active = MaiFfplayVideoView.activeView();
                assertNotNull(active);
                assertTrue("Speed change did not reuse VOD source", active.setSpeed(1.5));
                assertTrue("Replay did not reuse VOD source", active.replay());
            });
            assertTrue("VOD speed/replay blocked the UI thread",
                android.os.SystemClock.elapsedRealtime() - restartStarted < 4000);
            screen.onActivity(activity -> assertTrue(activity.commandAgentVideo("close", 0, 0, 0)));
            int fullDownloadsBeforeSave = state().getInt("full_downloads");
            Set<Long> savedBefore = savedCloudVideoIds(context);
            AccessibilityNodeInfo save = textNode(
                InstrumentationRegistry.getInstrumentation().getUiAutomation()
                    .getRootInActiveWindow(), "保存");
            assertNotNull("Cloud list needs an explicit Save action", save);
            assertTrue(save.performAction(AccessibilityNodeInfo.ACTION_CLICK));
            deadline = System.currentTimeMillis() + 30_000;
            Set<Long> newSaves = new HashSet<>();
            while (System.currentTimeMillis() < deadline) {
                newSaves = savedCloudVideoIds(context);
                newSaves.removeAll(savedBefore);
                if (!newSaves.isEmpty()) break;
                Thread.sleep(100);
            }
            assertFalse("Explicit cloud save did not reach MediaStore", newSaves.isEmpty());
            assertTrue("Explicit cloud save did not download the full object",
                state().getInt("full_downloads") > fullDownloadsBeforeSave);
            for (long savedID : newSaves)
                context.getContentResolver().delete(ContentUris.withAppendedId(
                    MediaStore.Video.Media.EXTERNAL_CONTENT_URI, savedID), null, null);
        }

        MaiCloudDrive.Result deleted = request(client, MaiCloudDrive.DELETE,
            new JSONObject().put("video_id", videoId));
        assertTrue(deleted.body, deleted.ok());
        assertEquals(0, request(client, MaiCloudDrive.LIST, new JSONObject())
            .json().getJSONArray("videos").length());
        assertEquals(404, request(client, MaiCloudDrive.PLAY_LINK,
            new JSONObject().put("video_id", videoId)).httpStatus);
        context.getContentResolver().delete(sourceUri, null, null);
        downloaded.delete();
    }

    @Test public void cloudTabRemainsVisibleWhenFeatureIsOff() throws Exception {
        assertTrue(BuildConfig.DEBUG && BuildConfig.USE_LOCAL_UI_AUTH &&
            BuildConfig.APPLICATION_ID.endsWith(".isolated"));
        Context context = ApplicationProvider.getApplicationContext();
        MaiChatAuthClient.shared(context).loginWithPassword("sync-ui-test", PASSWORD, true);
        MaiCloudDrive.useLocalFixtureForTest(null, certificateFrom(18190));
        try (ActivityScenario<MainActivity> screen = ActivityScenario.launch(MainActivity.class)) {
            screen.onActivity(activity -> {
                View tab = byDescription(activity.getWindow().getDecorView(), "云盘");
                assertNotNull(tab);
                assertTrue("云盘主标签始终可见", tab.isShown());
                tab.performClick();
            });
            Thread.sleep(900);
            screen.onActivity(activity -> {
                View root = activity.getWindow().getDecorView();
                assertNotNull("云盘页面应可访问", byDescription(root, "云盘状态"));
                View myTab = byDescription(root, "我的");
                assertNotNull(myTab);
                myTab.performClick();
                assertNull("我的页面不再重复云盘入口",
                    byDescription(activity.getWindow().getDecorView(), "打开我的云盘"));
            });
        }
    }

    @Test public void nonseekableProviderStagesThroughSharedCore() throws Exception {
        requireFixture();
        Context context = ApplicationProvider.getApplicationContext();
        MaiChatAuthClient.shared(context).loginWithPassword("sync-ui-test", PASSWORD, true);
        resetFixture();
        MaiCloudDrive client = new MaiCloudDrive(context);
        File checkpoint = new File(client.accountDirectory(), "android-cloud-pipe.maicloud.json");
        File staged = new File(checkpoint.getAbsolutePath() + ".source");
        checkpoint.delete();
        staged.delete();
        MaiCloudDrive.Source source = client.inspect(CloudPipeProvider.URI);
        assertEquals(CloudPipeProvider.LENGTH, source.bytes);
        MaiCloudDrive.Result saved = upload(client, source, checkpoint);
        assertTrue("Pipe upload failed status=" + saved.status + " http=" + saved.httpStatus +
            " body=" + saved.body, saved.ok());
        assertFalse(checkpoint.exists());
        assertFalse("Staged source must be cleaned after completion", staged.exists());
        JSONObject fixture = state();
        assertEquals(CloudPipeProvider.LENGTH, fixture.getLong("size"));
        assertEquals("ready", fixture.getString("status"));
        assertEquals(0, fixture.getInt("oss_authorization"));
        String videoId = request(client, MaiCloudDrive.LIST, new JSONObject())
            .json().getJSONArray("videos").getJSONObject(0).getString("id");
        assertTrue(request(client, MaiCloudDrive.DELETE,
            new JSONObject().put("video_id", videoId)).ok());
    }

    @Test public void pauseNonseekableUploadAfterDurableStage() throws Exception {
        requireFixture();
        Context context = ApplicationProvider.getApplicationContext();
        MaiChatAuthClient.shared(context).loginWithPassword("sync-ui-test", PASSWORD, true);
        resetFixture();
        MaiCloudDrive client = new MaiCloudDrive(context);
        File checkpoint = new File(client.accountDirectory(),
            PIPE_RESUME_ID + ".maicloud.json");
        File staged = new File(checkpoint.getAbsolutePath() + ".source");
        checkpoint.delete();
        staged.delete();
        MaiCloudDrive.Source source = client.inspect(CloudPipeProvider.URI);
        AtomicBoolean paused = new AtomicBoolean();
        client.upload(source, checkpoint, (done, total, staging) -> {
            if (!staging && done >= 8L * 1024 * 1024 && done < total &&
                paused.compareAndSet(false, true)) client.cancel();
        }, ignored -> { });
        long deadline = System.currentTimeMillis() + 90_000;
        while (!paused.get() && System.currentTimeMillis() < deadline)
            Thread.sleep(100);
        assertTrue("Pipe first part not observed", paused.get());
        JSONArray checkpoints = checkpoints(client).getJSONArray("uploads");
        boolean found = false;
        for (int i = 0; i < checkpoints.length(); ++i) {
            JSONObject item = checkpoints.getJSONObject(i);
            if (checkpoint.getAbsolutePath().equals(item.getString("checkpoint_path"))) {
                assertEquals(staged.getAbsolutePath(), item.getString("source_id"));
                found = true;
            }
        }
        assertTrue("Durable staged upload checkpoint missing", found);
        assertTrue(staged.isFile());
        assertEquals(CloudPipeProvider.LENGTH, staged.length());
        assertEquals(1, state().getInt("sessions"));
        assertEquals("uploading", state().getString("status"));
    }

    @Test public void resumeNonseekableAfterRelaunchWithoutRestaging() throws Exception {
        requireFixture();
        Context context = ApplicationProvider.getApplicationContext();
        MaiChatAuthClient.shared(context).loginWithPassword("sync-ui-test", PASSWORD, true);
        MaiCloudDrive client = new MaiCloudDrive(context);
        File checkpoint = new File(client.accountDirectory(),
            PIPE_RESUME_ID + ".maicloud.json");
        File staged = new File(checkpoint.getAbsolutePath() + ".source");
        assertTrue(checkpoint.isFile());
        assertTrue(staged.isFile());
        try (ActivityScenario<MainActivity> screen = ActivityScenario.launch(MainActivity.class)) {
            AtomicReference<AIAssistantController> owner = new AtomicReference<>();
            screen.onActivity(activity -> owner.set(AIAssistantController.shared(activity)));
            JSONObject accepted = host(owner.get(), "maichat_cloud_drive_resume_upload",
                new JSONObject().put("task_id", PIPE_RESUME_ID));
            assertTrue("Agent resume failed: " + accepted, accepted.getBoolean("ok"));
            assertTrue(accepted.getJSONObject("output").getBoolean("accepted"));
            long deadline = System.currentTimeMillis() + 15_000;
            while (!"ready".equals(state().getString("status")) &&
                System.currentTimeMillis() < deadline) Thread.sleep(100);
            assertEquals("ready", state().getString("status"));
            deadline = System.currentTimeMillis() + 10_000;
            while ((checkpoint.exists() || staged.exists()) &&
                System.currentTimeMillis() < deadline)
                Thread.sleep(50);
        }
        assertFalse(checkpoint.exists());
        assertFalse(staged.exists());
        assertEquals(1, state().getInt("sessions"));
        assertEquals("ready", state().getString("status"));
        String videoId = request(client, MaiCloudDrive.LIST, new JSONObject())
            .json().getJSONArray("videos").getJSONObject(0).getString("id");
        assertTrue(request(client, MaiCloudDrive.DELETE,
            new JSONObject().put("video_id", videoId)).ok());
    }

    @Test public void agentToolsRunOnWorkerAndPendingStaysResponsiveDuringUpload()
        throws Exception {
        requireFixture();
        Context context = ApplicationProvider.getApplicationContext();
        MaiChatAuthClient.shared(context).loginWithPassword("sync-ui-test", PASSWORD, true);
        resetFixture();
        try (ActivityScenario<MainActivity> screen = ActivityScenario.launch(MainActivity.class)) {
            AtomicReference<AIAssistantController> owner = new AtomicReference<>();
            screen.onActivity(activity -> owner.set(AIAssistantController.shared(activity)));
            File root = null;
            Field rootField = AIAssistantController.class.getDeclaredField("root");
            rootField.setAccessible(true);
            long deadline = System.currentTimeMillis() + 10_000;
            while (root == null && System.currentTimeMillis() < deadline) {
                root = (File) rootField.get(owner.get());
                if (root == null) Thread.sleep(50);
            }
            assertNotNull("Agent account workspace did not initialize", root);
            File workspace = new File(root, "Workspace");
            assertTrue(workspace.isDirectory() || workspace.mkdirs());
            File source = new File(workspace, "agent-cloud-e2e.mp4");
            try (InputStream input = InstrumentationRegistry.getInstrumentation()
                     .getContext().getAssets().open("gallery-sample.mp4");
                 FileOutputStream output = new FileOutputStream(source)) {
                byte[] buffer = new byte[8192];
                int count;
                while ((count = input.read(buffer)) >= 0) output.write(buffer, 0, count);
            }
            assertEquals(0, host(owner.get(), "maichat_cloud_drive_list_media",
                new JSONObject()).getJSONObject("output").getJSONArray("media").length());
            assertEquals(0, host(owner.get(), "maichat_cloud_drive_list_pending_uploads",
                new JSONObject()).getJSONObject("output").getJSONArray("uploads").length());
            JSONObject accepted = host(owner.get(), "maichat_cloud_drive_upload_media",
                new JSONObject().put("file_path", source.getName()));
            assertTrue(accepted.toString(), accepted.getBoolean("ok"));
            String task = accepted.getJSONObject("output").getString("task_id");
            assertTrue(accepted.getJSONObject("output").getBoolean("accepted"));
            JSONObject activeAbort = host(owner.get(), "maichat_cloud_drive_abort_upload",
                new JSONObject().put("task_id", task));
            assertFalse(activeAbort.toString(), activeAbort.getBoolean("ok"));
            assertEquals("busy", activeAbort.getString("errorCode"));
            long pendingStarted = android.os.SystemClock.elapsedRealtime();
            JSONObject pending = host(owner.get(), "maichat_cloud_drive_list_pending_uploads",
                new JSONObject());
            assertTrue("Pending request queued behind upload", android.os.SystemClock
                .elapsedRealtime() - pendingStarted < 1800);
            boolean seen = false;
            JSONArray uploads = pending.getJSONObject("output").getJSONArray("uploads");
            for (int i = 0; i < uploads.length(); ++i)
                if (task.equals(uploads.getJSONObject(i).optString("task_id"))) seen = true;
            assertTrue("Agent upload missing from pending view", seen);
            deadline = System.currentTimeMillis() + 15_000;
            while (!"ready".equals(state().getString("status")) &&
                System.currentTimeMillis() < deadline) Thread.sleep(100);
            assertEquals("ready", state().getString("status"));
            JSONArray videos = host(owner.get(), "maichat_cloud_drive_list_media",
                new JSONObject()).getJSONObject("output").getJSONArray("media");
            assertEquals(1, videos.length());
            String videoId = videos.getJSONObject(0).getString("media_id");
            JSONObject opened = host(owner.get(), "maichat_cloud_drive_open_media",
                new JSONObject().put("media_id", videoId));
            assertTrue(opened.toString(), opened.getBoolean("ok"));
            assertTrue(opened.getJSONObject("output").getBoolean("opened"));
            String agentPlayback = null;
            deadline = System.currentTimeMillis() + 10_000;
            while (System.currentTimeMillis() < deadline) {
                MaiFfplayVideoView active = MaiFfplayVideoView.activeView();
                if (active != null && active.hasPresentedFrame() &&
                    active.currentVideoPath() != null &&
                    active.currentVideoPath().startsWith("maicloud-vod-")) {
                    agentPlayback = active.currentVideoPath();
                    break;
                }
                Thread.sleep(100);
            }
            assertNotNull("Agent cloud player did not open", agentPlayback);
            assertFalse("Agent cloud playback wrote a media file",
                new File(agentPlayback).exists());
            assertTrue("Agent cloud playback did not use Range",
                state().getInt("range_requests") > 0);
            Thread.sleep(400);
            screenshot(context, "cloud-drive-agent-vod-playing.png");
            screen.onActivity(activity -> assertTrue(activity.commandAgentVideo("close", 0, 0, 0)));
            assertTrue(host(owner.get(), "maichat_cloud_drive_delete_media",
                new JSONObject().put("media_id", videoId))
                .getJSONObject("output").getBoolean("deleted"));
            File imageSource = new File(workspace, "agent-cloud-image.png");
            Bitmap pixels = Bitmap.createBitmap(48, 32, Bitmap.Config.ARGB_8888);
            pixels.eraseColor(0xff31a7df);
            try (FileOutputStream output = new FileOutputStream(imageSource)) {
                assertTrue(pixels.compress(Bitmap.CompressFormat.PNG, 100, output));
            } finally { pixels.recycle(); }
            assertTrue(host(owner.get(), "maichat_cloud_drive_upload_media",
                new JSONObject().put("file_path", imageSource.getName())).getBoolean("ok"));
            deadline = System.currentTimeMillis() + 15_000;
            while (!"ready".equals(state().getString("status")) &&
                System.currentTimeMillis() < deadline) Thread.sleep(100);
            JSONArray media = host(owner.get(), "maichat_cloud_drive_list_media",
                new JSONObject()).getJSONObject("output").getJSONArray("media");
            assertEquals(1, media.length());
            assertEquals("image", media.getJSONObject(0).getString("media_type"));
            String imageId = media.getJSONObject(0).getString("media_id");
            JSONObject imageOpened = host(owner.get(), "maichat_cloud_drive_open_media",
                new JSONObject().put("media_id", imageId));
            assertTrue(imageOpened.toString(), imageOpened.getBoolean("ok"));
            assertEquals("image", imageOpened.getJSONObject("output")
                .getString("media_type"));
            AtomicReference<File> agentImage = new AtomicReference<>();
            screen.onActivity(activity -> {
                try {
                    Field dialogField = MainActivity.class.getDeclaredField("imageDialog");
                    dialogField.setAccessible(true);
                    android.app.Dialog dialog = (android.app.Dialog) dialogField.get(activity);
                    assertNotNull("Agent image viewer did not open", dialog);
                    assertTrue(dialog.isShowing());
                    Field filesField = MainActivity.class.getDeclaredField("cloudImageFiles");
                    filesField.setAccessible(true);
                    Map<?, ?> files = (Map<?, ?>) filesField.get(activity);
                    agentImage.set((File) files.get(dialog));
                    assertNotNull("Agent image file was not tracked", agentImage.get());
                    assertNotNull("Agent image file did not decode",
                        BitmapFactory.decodeFile(agentImage.get().getAbsolutePath()));
                    dialog.dismiss();
                } catch (Exception failure) { throw new AssertionError(failure); }
            });
            assertNotNull("Agent image download was not owned by the viewer", agentImage.get());
            deadline = System.currentTimeMillis() + 5_000;
            while (agentImage.get().exists() && System.currentTimeMillis() < deadline)
                Thread.sleep(50);
            assertFalse("Agent image cache survived preview close", agentImage.get().exists());
            assertTrue(host(owner.get(), "maichat_cloud_drive_delete_media",
                new JSONObject().put("media_id", imageId))
                .getJSONObject("output").getBoolean("deleted"));
            imageSource.delete();
            source.delete();
        }
    }

    @Test public void agentPauseThenResumeUsesSameUploadTask() throws Exception {
        requireFixture();
        Context context = ApplicationProvider.getApplicationContext();
        MaiChatAuthClient.shared(context).loginWithPassword("sync-ui-test", PASSWORD, true);
        resetFixture();
        try (ActivityScenario<MainActivity> screen = ActivityScenario.launch(MainActivity.class)) {
            AtomicReference<AIAssistantController> owner = new AtomicReference<>();
            screen.onActivity(activity -> owner.set(AIAssistantController.shared(activity)));
            Field rootField = AIAssistantController.class.getDeclaredField("root");
            rootField.setAccessible(true);
            File root = null;
            long deadline = System.currentTimeMillis() + 10_000;
            while (root == null && System.currentTimeMillis() < deadline) {
                root = (File) rootField.get(owner.get());
                if (root == null) Thread.sleep(50);
            }
            assertNotNull(root);
            File workspace = new File(root, "Workspace");
            assertTrue(workspace.isDirectory() || workspace.mkdirs());
            File source = new File(workspace, "agent-pause-resume.mp4");
            try (InputStream input = InstrumentationRegistry.getInstrumentation()
                     .getContext().getAssets().open("gallery-sample.mp4");
                 FileOutputStream output = new FileOutputStream(source)) {
                byte[] buffer = new byte[8192];
                int count;
                while ((count = input.read(buffer)) >= 0) output.write(buffer, 0, count);
            }
            try (RandomAccessFile data = new RandomAccessFile(source, "rw")) {
                data.setLength(VIDEO_BYTES);
            }
            JSONObject accepted = host(owner.get(), "maichat_cloud_drive_upload_media",
                new JSONObject().put("file_path", source.getName()));
            assertTrue(accepted.toString(), accepted.getBoolean("ok"));
            String task = accepted.getJSONObject("output").getString("task_id");
            deadline = System.currentTimeMillis() + 20_000;
            while (state().getJSONArray("parts").length() == 0 &&
                System.currentTimeMillis() < deadline) Thread.sleep(50);
            assertTrue("First OSS part was not reached", state().getJSONArray("parts").length() > 0);
            JSONObject pausing = host(owner.get(), "maichat_cloud_drive_pause_upload",
                new JSONObject().put("task_id", task));
            assertTrue(pausing.toString(), pausing.getBoolean("ok"));
            assertEquals("pausing", pausing.getJSONObject("output").getString("state"));
            deadline = System.currentTimeMillis() + 15_000;
            String status = "";
            while (System.currentTimeMillis() < deadline) {
                JSONArray uploads = host(owner.get(),
                    "maichat_cloud_drive_list_pending_uploads", new JSONObject())
                    .getJSONObject("output").getJSONArray("uploads");
                for (int i = 0; i < uploads.length(); ++i)
                    if (task.equals(uploads.getJSONObject(i).optString("task_id")))
                        status = uploads.getJSONObject(i).optString("state");
                if ("paused".equals(status)) break;
                Thread.sleep(100);
            }
            assertEquals("paused", status);
            JSONObject paused = host(owner.get(), "maichat_cloud_drive_pause_upload",
                new JSONObject().put("task_id", task));
            assertTrue(paused.toString(), paused.getBoolean("ok"));
            assertTrue(paused.getJSONObject("output").getBoolean("paused"));
            JSONObject resumed = host(owner.get(), "maichat_cloud_drive_resume_upload",
                new JSONObject().put("task_id", task));
            assertTrue(resumed.toString(), resumed.getBoolean("ok"));
            assertTrue(resumed.getJSONObject("output").getBoolean("accepted"));
            deadline = System.currentTimeMillis() + 30_000;
            while (!"ready".equals(state().getString("status")) &&
                System.currentTimeMillis() < deadline) Thread.sleep(100);
            assertEquals("ready", state().getString("status"));
            MaiCloudDrive client = new MaiCloudDrive(context);
            File checkpoint = new File(client.accountDirectory(), task + ".maicloud.json");
            deadline = System.currentTimeMillis() + 10_000;
            while (checkpoint.exists() && System.currentTimeMillis() < deadline)
                Thread.sleep(50);
            assertFalse(checkpoint.exists());
            assertEquals(1, state().getInt("sessions"));
            String videoId = request(client, MaiCloudDrive.LIST, new JSONObject())
                .json().getJSONArray("videos").getJSONObject(0).getString("id");
            assertTrue(request(client, MaiCloudDrive.DELETE,
                new JSONObject().put("video_id", videoId)).ok());
            source.delete();
        }
    }

    @Test public void agentAbortPausedPipeUploadRemovesOnlyPrivateStage()
        throws Exception {
        requireFixture();
        Context context = ApplicationProvider.getApplicationContext();
        MaiChatAuthClient.shared(context).loginWithPassword("sync-ui-test", PASSWORD, true);
        resetFixture();
        MaiCloudDrive client = new MaiCloudDrive(context);
        File checkpoint = client.newCheckpoint();
        File staged = new File(checkpoint.getAbsolutePath() + ".source");
        AtomicBoolean requestedPause = new AtomicBoolean();
        CountDownLatch settled = new CountDownLatch(1);
        client.upload(client.inspect(CloudPipeProvider.URI), checkpoint,
            (done, total, staging) -> {
                if (!staging && done >= 8L * 1024 * 1024 && done < total &&
                    requestedPause.compareAndSet(false, true)) client.cancel();
            }, result -> { }, settled::countDown);
        assertTrue("Core upload did not settle after pause", settled.await(90, TimeUnit.SECONDS));
        assertTrue(requestedPause.get());
        assertTrue(checkpoint.isFile());
        assertTrue(staged.isFile());
        String task = checkpoint.getName().replace(".maicloud.json", "");
        try (ActivityScenario<MainActivity> screen = ActivityScenario.launch(MainActivity.class)) {
            AtomicReference<AIAssistantController> owner = new AtomicReference<>();
            screen.onActivity(activity -> owner.set(AIAssistantController.shared(activity)));
            JSONObject aborted = host(owner.get(), "maichat_cloud_drive_abort_upload",
                new JSONObject().put("task_id", task));
            assertTrue(aborted.toString(), aborted.getBoolean("ok"));
            assertTrue(aborted.getJSONObject("output").getBoolean("aborted"));
            assertEquals(task, aborted.getJSONObject("output").getString("task_id"));
            JSONObject again = host(owner.get(), "maichat_cloud_drive_abort_upload",
                new JSONObject().put("task_id", task));
            assertFalse(again.toString(), again.getBoolean("ok"));
            assertEquals("not_found", again.getString("errorCode"));
        }
        assertFalse(checkpoint.exists());
        assertFalse("App-owned source remained after confirmed abort", staged.exists());
        assertEquals("deleted", state().getString("status"));
        assertEquals(CloudPipeProvider.LENGTH, client.inspect(CloudPipeProvider.URI).bytes);
    }

    @Test public void accountRoundTripWaitsForOldUploadBeforeSameTaskResume()
        throws Exception {
        requireFixture();
        Context context = ApplicationProvider.getApplicationContext();
        MaiChatAuthClient auth = MaiChatAuthClient.shared(context);
        String ownerUser = auth.loginWithPassword("sync-ui-test", PASSWORD, true);
        String account = auth.accountId();
        resetFixture();
        try (ActivityScenario<MainActivity> screen = ActivityScenario.launch(MainActivity.class)) {
            AtomicReference<MainActivity> activityRef = new AtomicReference<>();
            AtomicReference<AIAssistantController> owner = new AtomicReference<>();
            screen.onActivity(activity -> {
                activityRef.set(activity);
                owner.set(AIAssistantController.shared(activity));
            });
            AIAssistantController.configureAccount(ownerUser);
            owner.set(AIAssistantController.shared(activityRef.get()));
            Field rootField = AIAssistantController.class.getDeclaredField("root");
            rootField.setAccessible(true);
            File root = null;
            long deadline = System.currentTimeMillis() + 10_000;
            while (root == null && System.currentTimeMillis() < deadline) {
                root = (File) rootField.get(owner.get());
                if (root == null) Thread.sleep(50);
            }
            assertNotNull(root);
            File workspace = new File(root, "Workspace");
            assertTrue(workspace.isDirectory() || workspace.mkdirs());
            File source = new File(workspace, "agent-fast-account-switch.mp4");
            try (InputStream input = InstrumentationRegistry.getInstrumentation()
                     .getContext().getAssets().open("gallery-sample.mp4");
                 FileOutputStream output = new FileOutputStream(source)) {
                byte[] buffer = new byte[8192];
                int count;
                while ((count = input.read(buffer)) >= 0) output.write(buffer, 0, count);
            }
            try (RandomAccessFile data = new RandomAccessFile(source, "rw")) {
                data.setLength(VIDEO_BYTES);
            }
            JSONObject accepted = MaiCloudDriveAgentTools.execute(activityRef.get(),
                "maichat_cloud_drive_upload_media",
                new JSONObject().put("file_path", source.getName()));
            String task = accepted.getString("task_id");
            Field activeField = MaiCloudDriveAgentTools.class.getDeclaredField("active");
            activeField.setAccessible(true);
            Map<?, ?> active = (Map<?, ?>) activeField.get(null);
            Object tracked = active.get(account + ":" + task);
            assertNotNull(tracked);
            Field settledField = tracked.getClass().getDeclaredField("settled");
            settledField.setAccessible(true);
            deadline = System.currentTimeMillis() + 20_000;
            while (state().getJSONArray("parts").length() == 0 &&
                System.currentTimeMillis() < deadline) Thread.sleep(50);
            assertTrue(state().getJSONArray("parts").length() > 0);
            String viewerUser = auth.loginWithPassword("sync-ui-viewer", PASSWORD, true);
            AIAssistantController.configureAccount(viewerUser);
            auth.loginWithPassword("sync-ui-test", PASSWORD, true);
            AIAssistantController.configureAccount(ownerUser);
            assertEquals(account, auth.accountId());
            boolean earlyResumed = false;
            if (!settledField.getBoolean(tracked)) {
                assertTrue("Account switch released an active checkpoint early",
                    active.get(account + ":" + task) == tracked);
                try {
                    JSONObject early = MaiCloudDriveAgentTools.execute(activityRef.get(),
                        "maichat_cloud_drive_resume_upload",
                        new JSONObject().put("task_id", task));
                    assertTrue("Resume began before old upload settled",
                        settledField.getBoolean(tracked));
                    assertTrue(early.getBoolean("accepted"));
                    earlyResumed = true;
                } catch (MaiCloudDriveAgentTools.Failure failure) {
                    assertEquals("busy", failure.code);
                }
            }
            deadline = System.currentTimeMillis() + 15_000;
            while (!settledField.getBoolean(tracked) &&
                System.currentTimeMillis() < deadline) Thread.sleep(50);
            assertTrue("Old upload worker never settled", settledField.getBoolean(tracked));
            if (!earlyResumed) assertFalse(active.containsKey(account + ":" + task));
            MaiCloudDrive client = new MaiCloudDrive(context);
            File checkpoint = new File(client.accountDirectory(), task + ".maicloud.json");
            if (!earlyResumed) {
                assertTrue("Canceled upload lost its resumable checkpoint", checkpoint.exists());
                JSONObject resumed = MaiCloudDriveAgentTools.execute(activityRef.get(),
                    "maichat_cloud_drive_resume_upload",
                    new JSONObject().put("task_id", task));
                assertTrue(resumed.getBoolean("accepted"));
            }
            deadline = System.currentTimeMillis() + 30_000;
            while ((checkpoint.exists() || !"ready".equals(state().getString("status"))) &&
                System.currentTimeMillis() < deadline) Thread.sleep(100);
            assertFalse(checkpoint.exists());
            assertEquals("ready", state().getString("status"));
            assertEquals("Quick account switch created a second upload session",
                1, state().getInt("sessions"));
            String videoId = request(client, MaiCloudDrive.LIST, new JSONObject())
                .json().getJSONArray("videos").getJSONObject(0).getString("id");
            assertTrue(request(client, MaiCloudDrive.DELETE,
                new JSONObject().put("video_id", videoId)).ok());
            source.delete();
        }
    }

    @Test public void agentFoldersReuseNormalClientAndFilterMedia() throws Exception {
        requireFixture();
        resetFixture();
        Context context = ApplicationProvider.getApplicationContext();
        MaiChatAuthClient auth = MaiChatAuthClient.shared(context);
        auth.loginWithPassword("sync-ui-test", PASSWORD, true);
        MaiCloudDrive client = new MaiCloudDrive(context);
        File source = new File(context.getCacheDir(), "folder-tool-video.mp4");
        try (InputStream input = InstrumentationRegistry.getInstrumentation()
                 .getContext().getAssets().open("gallery-sample.mp4");
             FileOutputStream output = new FileOutputStream(source)) {
            byte[] bytes = new byte[8192];
            int count;
            while ((count = input.read(bytes)) != -1) output.write(bytes, 0, count);
        }
        MaiCloudDrive.Result uploaded = upload(client, client.inspect(source), client.newCheckpoint());
        assertTrue("upload status=" + uploaded.status + " http=" + uploaded.httpStatus,
                   uploaded.ok());
        String mediaId = request(client, MaiCloudDrive.LIST, new JSONObject())
            .json().getJSONArray("videos").getJSONObject(0).getString("id");
        try (ActivityScenario<MainActivity> screen = ActivityScenario.launch(MainActivity.class)) {
            AtomicReference<AIAssistantController> owner = new AtomicReference<>();
            screen.onActivity(activity -> owner.set(AIAssistantController.shared(activity)));
            JSONObject created = host(owner.get(), "maichat_cloud_drive_create_folder",
                new JSONObject().put("name", "工作资料"));
            assertTrue(created.toString(), created.getBoolean("ok"));
            JSONObject parent = created.getJSONObject("output");
            String parentId = parent.getString("id");
            assertTrue(parent.isNull("parent_id"));
            JSONObject nested = host(owner.get(), "maichat_cloud_drive_create_folder",
                new JSONObject().put("name", "项目一").put("parent_id", parentId));
            assertTrue(nested.toString(), nested.getBoolean("ok"));
            String childId = nested.getJSONObject("output").getString("id");
            JSONObject renamed = host(owner.get(), "maichat_cloud_drive_update_folder",
                new JSONObject().put("folder_id", childId).put("name", "项目二"));
            assertTrue(renamed.toString(), renamed.getBoolean("ok"));
            assertEquals("项目二", renamed.getJSONObject("output").getString("name"));
            JSONObject moved = host(owner.get(), "maichat_cloud_drive_move_media",
                new JSONObject().put("media_id", mediaId).put("folder_id", childId));
            assertTrue(moved.toString(), moved.getBoolean("ok"));
            JSONObject named = host(owner.get(), "maichat_cloud_drive_rename_media",
                new JSONObject().put("media_id", mediaId).put("name", "项目影片.MP4"));
            assertTrue(named.toString(), named.getBoolean("ok"));
            JSONObject renamedMedia = named.getJSONObject("output");
            assertEquals("项目影片.MP4", renamedMedia.getString("name"));
            assertEquals(childId, renamedMedia.getString("folder_id"));
            assertFalse(renamedMedia.toString().contains("token"));
            assertFalse(renamedMedia.toString().contains("url"));
            JSONObject normalMedia = request(client, MaiCloudDrive.LIST,
                new JSONObject().put("folder_id", childId)).json()
                .getJSONArray("videos").getJSONObject(0);
            assertEquals("项目影片.MP4", normalMedia.getString("name"));
            CountDownLatch renameDone = new CountDownLatch(1);
            AtomicReference<MaiCloudDrive.Result> normalRename = new AtomicReference<>();
            client.renameMedia(mediaId, "归档影片.mp4", value -> {
                normalRename.set(value); renameDone.countDown();
            });
            assertTrue(renameDone.await(30, TimeUnit.SECONDS));
            assertTrue(normalRename.get().body, normalRename.get().ok());
            assertEquals("归档影片.mp4", normalRename.get().json().getString("name"));
            JSONObject invalidRename = host(owner.get(), "maichat_cloud_drive_rename_media",
                new JSONObject().put("media_id", mediaId).put("name", "不能转换.mov"));
            assertFalse(invalidRename.toString(), invalidRename.getBoolean("ok"));
            assertEquals("invalid_input", invalidRename.getString("errorCode"));
            JSONObject filtered = host(owner.get(), "maichat_cloud_drive_list_media",
                new JSONObject().put("folder_id", childId));
            assertTrue(filtered.toString(), filtered.getBoolean("ok"));
            JSONArray media = filtered.getJSONObject("output").getJSONArray("media");
            assertEquals(1, media.length());
            assertEquals(childId, media.getJSONObject(0).getString("folder_id"));
            assertEquals("归档影片.mp4", media.getJSONObject(0).getString("name"));
            assertFalse(filtered.getJSONObject("output").toString().contains("read_url"));
            assertFalse(filtered.getJSONObject("output").toString().contains("token"));
            JSONObject root = host(owner.get(), "maichat_cloud_drive_list_media",
                new JSONObject().put("folder_id", ""));
            assertEquals(0, root.getJSONObject("output").getJSONArray("media").length());
            JSONObject blocked = host(owner.get(), "maichat_cloud_drive_delete_folder",
                new JSONObject().put("folder_id", childId));
            assertFalse(blocked.toString(), blocked.getBoolean("ok"));
            assertEquals("conflict", blocked.getString("errorCode"));
            CountDownLatch done = new CountDownLatch(1);
            AtomicReference<MaiCloudDrive.Result> normal = new AtomicReference<>();
            client.listFolders(value -> { normal.set(value); done.countDown(); });
            assertTrue(done.await(30, TimeUnit.SECONDS));
            assertTrue(normal.get().body, normal.get().ok());
            assertEquals(2, normal.get().json().getJSONArray("folders").length());
            JSONObject listed = host(owner.get(), "maichat_cloud_drive_list_folders", new JSONObject());
            assertTrue(listed.toString(), listed.getBoolean("ok"));
            assertFalse(listed.getJSONObject("output").toString().contains("token"));
            assertFalse(listed.getJSONObject("output").toString().contains("url"));
            assertEquals(2, listed.getJSONObject("output").getJSONArray("folders").length());
            JSONObject ungrouped = host(owner.get(), "maichat_cloud_drive_move_media",
                new JSONObject().put("media_id", mediaId).put("folder_id", ""));
            assertTrue(ungrouped.toString(), ungrouped.getBoolean("ok"));
            for (String folder : new String[]{childId, parentId}) {
                JSONObject deleted = host(owner.get(), "maichat_cloud_drive_delete_folder",
                    new JSONObject().put("folder_id", folder));
                assertTrue(deleted.toString(), deleted.getBoolean("ok"));
            }
            assertTrue(request(client, MaiCloudDrive.DELETE,
                new JSONObject().put("video_id", mediaId)).ok());
            screen.onActivity(activity -> {
                View cloud = byDescription(activity.getWindow().getDecorView(), "云盘");
                assertNotNull(cloud);
                cloud.performClick();
            });
            Thread.sleep(1_000);
            screenshot(context, "cloud-drive-folders-agent-only.png");
        } finally { source.delete(); }
    }

    @Test public void agentCloudDisabledMapsToNotConfigured() throws Exception {
        assertTrue(BuildConfig.DEBUG && BuildConfig.USE_LOCAL_UI_AUTH &&
            BuildConfig.APPLICATION_ID.endsWith(".isolated"));
        Context context = ApplicationProvider.getApplicationContext();
        MaiChatAuthClient.shared(context).loginWithPassword("sync-ui-test", PASSWORD, true);
        MaiCloudDrive.useLocalFixtureForTest(null, certificateFrom(18190));
        try (ActivityScenario<MainActivity> screen = ActivityScenario.launch(MainActivity.class)) {
            AtomicReference<AIAssistantController> owner = new AtomicReference<>();
            screen.onActivity(activity -> owner.set(AIAssistantController.shared(activity)));
            JSONObject failed = host(owner.get(), "maichat_cloud_drive_list_media",
                new JSONObject());
            assertFalse(failed.toString(), failed.getBoolean("ok"));
            assertEquals("not_configured", failed.getString("errorCode"));
        }
    }

    private static Uri makeVideo(Context context) throws Exception {
        File source = new File(context.getCacheDir(), "cloud-drive-source.mp4");
        try (InputStream input = InstrumentationRegistry.getInstrumentation()
                 .getContext().getAssets().open("gallery-sample.mp4");
             FileOutputStream output = new FileOutputStream(source)) {
            byte[] buffer = new byte[8192];
            int count;
            while ((count = input.read(buffer)) >= 0) output.write(buffer, 0, count);
        }
        try (RandomAccessFile data = new RandomAccessFile(source, "rw")) {
            data.setLength(VIDEO_BYTES);
        }
        ContentValues values = new ContentValues();
        values.put(MediaStore.Video.Media.DISPLAY_NAME, "cloud-resume-test.mp4");
        values.put(MediaStore.Video.Media.MIME_TYPE, "video/mp4");
        values.put(MediaStore.Video.Media.RELATIVE_PATH, "Movies/MaiChatCloudFixture");
        values.put(MediaStore.Video.Media.IS_PENDING, 1);
        Uri uri = context.getContentResolver().insert(
            MediaStore.Video.Media.EXTERNAL_CONTENT_URI, values);
        assertNotNull(uri);
        try (InputStream input = new java.io.FileInputStream(source);
             java.io.OutputStream output = context.getContentResolver().openOutputStream(uri)) {
            assertNotNull(output);
            byte[] buffer = new byte[128 * 1024];
            int count;
            while ((count = input.read(buffer)) >= 0) output.write(buffer, 0, count);
        }
        values.clear();
        values.put(MediaStore.Video.Media.IS_PENDING, 0);
        context.getContentResolver().update(uri, values, null, null);
        source.delete();
        return uri;
    }

    private static MaiCloudDrive.Result upload(MaiCloudDrive client,
                                               MaiCloudDrive.Source source,
                                               File checkpoint) throws Exception {
        CountDownLatch done = new CountDownLatch(1);
        AtomicReference<MaiCloudDrive.Result> result = new AtomicReference<>();
        client.upload(source, checkpoint, (bytes, total, staging) -> { }, value -> {
            result.set(value); done.countDown();
        });
        assertTrue("Cloud upload timed out", done.await(90, TimeUnit.SECONDS));
        assertNotNull(result.get());
        return result.get();
    }

    private static MaiCloudDrive.Result request(MaiCloudDrive client, int action,
                                                JSONObject payload) throws Exception {
        CountDownLatch done = new CountDownLatch(1);
        AtomicReference<MaiCloudDrive.Result> result = new AtomicReference<>();
        client.request(action, payload, value -> { result.set(value); done.countDown(); });
        assertTrue("Cloud request timed out", done.await(30, TimeUnit.SECONDS));
        assertNotNull(result.get());
        return result.get();
    }

    private static JSONObject checkpoints(MaiCloudDrive client) throws Exception {
        CountDownLatch done = new CountDownLatch(1);
        AtomicReference<MaiCloudDrive.Result> result = new AtomicReference<>();
        client.checkpoints(value -> { result.set(value); done.countDown(); });
        assertTrue("Checkpoint query timed out", done.await(30, TimeUnit.SECONDS));
        assertNotNull(result.get());
        assertTrue(result.get().body, result.get().ok());
        return result.get().json();
    }

    private static JSONObject host(AIAssistantController controller, String tool,
                                   JSONObject arguments) throws Exception {
        Method callback = AIAssistantController.class.getDeclaredMethod("onNativeHostTool",
            byte[].class, byte[].class);
        callback.setAccessible(true);
        byte[] bytes = (byte[]) callback.invoke(controller,
            tool.getBytes(java.nio.charset.StandardCharsets.UTF_8),
            arguments.toString().getBytes(java.nio.charset.StandardCharsets.UTF_8));
        return new JSONObject(new String(bytes, java.nio.charset.StandardCharsets.UTF_8));
    }

    private static void requireFixture() throws Exception {
        assertTrue("Isolated local test build only", BuildConfig.DEBUG &&
            BuildConfig.USE_LOCAL_UI_AUTH && BuildConfig.APPLICATION_ID.endsWith(".isolated"));
        MaiCloudDrive.useLocalFixtureForTest("https://localhost:18446", cert());
    }

    private static byte[] cert() throws Exception {
        return certificateFrom(18447);
    }

    private static byte[] certificateFrom(int port) throws Exception {
        HttpURLConnection connection = (HttpURLConnection)
            new URL("http://10.0.2.2:" + port + "/test/cert").openConnection();
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

    private static JSONObject state() throws Exception {
        HttpURLConnection connection = (HttpURLConnection)
            new URL("http://10.0.2.2:18447/test/state").openConnection();
        connection.setConnectTimeout(5000);
        connection.setReadTimeout(5000);
        try (InputStream input = connection.getInputStream();
             ByteArrayOutputStream output = new ByteArrayOutputStream()) {
            byte[] buffer = new byte[4096];
            int count;
            while ((count = input.read(buffer)) != -1) output.write(buffer, 0, count);
            return new JSONObject(output.toString(java.nio.charset.StandardCharsets.UTF_8.name()));
        } finally { connection.disconnect(); }
    }

    private static void resetFixture() throws Exception {
        HttpURLConnection connection = (HttpURLConnection)
            new URL("http://10.0.2.2:18447/test/reset").openConnection();
        connection.setRequestMethod("POST");
        connection.setConnectTimeout(5000);
        connection.setReadTimeout(5000);
        assertEquals(204, connection.getResponseCode());
        connection.disconnect();
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

    private static AccessibilityNodeInfo textNode(AccessibilityNodeInfo root, String wanted) {
        if (root.getText() != null && wanted.contentEquals(root.getText())) return root;
        for (int i = 0; i < root.getChildCount(); ++i) {
            AccessibilityNodeInfo child = root.getChild(i);
            if (child != null) {
                AccessibilityNodeInfo found = textNode(child, wanted);
                if (found != null) return found;
            }
        }
        return null;
    }

    private static void screenshot(Context context, String name) throws Exception {
        Bitmap image = InstrumentationRegistry.getInstrumentation().getUiAutomation()
            .takeScreenshot();
        assertNotNull(image);
        File file = new File(context.getExternalFilesDir(null), name);
        try (FileOutputStream output = new FileOutputStream(file)) {
            assertTrue(image.compress(Bitmap.CompressFormat.PNG, 100, output));
        }
        image.recycle();
    }

    private static Set<Long> savedCloudVideoIds(Context context) {
        Set<Long> ids = new HashSet<>();
        try (Cursor rows = context.getContentResolver().query(
            MediaStore.Video.Media.EXTERNAL_CONTENT_URI,
            new String[]{MediaStore.Video.Media._ID},
            MediaStore.Video.Media.DISPLAY_NAME + " LIKE ?",
            new String[]{"MaiChat-%"}, null)) {
            if (rows != null) while (rows.moveToNext()) ids.add(rows.getLong(0));
        }
        return ids;
    }
}
