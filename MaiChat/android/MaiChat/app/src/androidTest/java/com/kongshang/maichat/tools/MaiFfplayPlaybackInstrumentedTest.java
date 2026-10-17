// 文件作用：Android 设备集成测试；MaiFfplayPlaybackInstrumentedTest
// 负责覆盖对应界面或服务的设备行为，本文件给出其验证。 运行边界：仅供自动化验证使用，不作为正式 App 或 Agent
// 的用户功能入口。
// 覆盖场景：swipingGalleryMovesFromImageToVideoAndBack、swipingActiveVideoMovesBetweenConversationFiles、
// agentOpensControlsAndClosesGraphicsVideo、writeImage。
// 直接依赖：com.kongshang.maichat.AIAssistantController、com.kongshang.maichat.MainActivity、
// com.kongshang.maichat.MaiFfplayVideoView；实现与这些接口协作。
package com.kongshang.maichat.tools;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertSame;
import static org.junit.Assert.assertTrue;

import android.app.Activity;
import android.app.UiAutomation;
import android.content.Context;
import android.content.Intent;
import android.content.res.Configuration;
import android.database.Cursor;
import android.graphics.Bitmap;
import android.graphics.Color;
import android.os.SystemClock;
import android.provider.MediaStore;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.accessibility.AccessibilityNodeInfo;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import com.kongshang.maichat.AIAssistantController;
import com.kongshang.maichat.MainActivity;
import com.kongshang.maichat.MaiFfplayVideoView;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.lang.reflect.Field;
import java.util.Arrays;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import org.json.JSONObject;
import org.junit.Test;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
public final class MaiFfplayPlaybackInstrumentedTest {
    @Test public void swipingGalleryMovesFromImageToVideoAndBack() throws Exception {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        Activity activity = InstrumentationRegistry.getInstrumentation().startActivitySync(
            new Intent(context, MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        File first = AIAssistantController.shared(activity).workspaceFile("swipe-image-first.png");
        File second = AIAssistantController.shared(activity).workspaceFile("swipe-image-second.png");
        File video = AIAssistantController.shared(activity).workspaceFile("swipe-middle.mp4");
        assertNotNull(first);
        assertNotNull(second);
        assertNotNull(video);
        try {
            writeImage(first, Color.RED);
            writeImage(second, Color.BLUE);
            try (InputStream input = InstrumentationRegistry.getInstrumentation()
                    .getContext().getAssets().open("gallery-sample.mp4");
                 FileOutputStream output = new FileOutputStream(video)) {
                byte[] buffer = new byte[8192];
                int count;
                while ((count = input.read(buffer)) >= 0) output.write(buffer, 0, count);
            }
            java.util.List<MainActivity.GalleryItem> gallery = Arrays.asList(
                new MainActivity.GalleryItem(first.getAbsolutePath(), false),
                new MainActivity.GalleryItem(video.getAbsolutePath(), true),
                new MainActivity.GalleryItem(second.getAbsolutePath(), false));
            InstrumentationRegistry.getInstrumentation().runOnMainSync(() ->
                ((MainActivity) activity).openAgentMedia(first.getAbsolutePath(), false, gallery));
            awaitGalleryName("swipe-image-first.png");
            swipeScreen(true);
            awaitVideo(video.getAbsolutePath());
            swipeActiveVideo(true);
            awaitGalleryName("swipe-image-second.png");
            swipeScreen(false);
            awaitVideo(video.getAbsolutePath());
        } finally {
            InstrumentationRegistry.getInstrumentation().runOnMainSync(activity::finish);
            first.delete();
            second.delete();
            video.delete();
        }
    }

    private static void writeImage(File file, int color) throws Exception {
        Bitmap bitmap = Bitmap.createBitmap(320, 500, Bitmap.Config.ARGB_8888);
        bitmap.eraseColor(color);
        try (FileOutputStream output = new FileOutputStream(file)) {
            assertTrue(bitmap.compress(Bitmap.CompressFormat.PNG, 100, output));
        } finally { bitmap.recycle(); }
    }

    @Test public void adjacentImagesKeepOnePreviewWindowAndShowBothImages() throws Exception {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        Activity activity = InstrumentationRegistry.getInstrumentation().startActivitySync(
            new Intent(context, MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        File first = AIAssistantController.shared(activity).workspaceFile("gallery-red.png");
        File second = AIAssistantController.shared(activity).workspaceFile("gallery-blue.png");
        assertNotNull(first);
        assertNotNull(second);
        try {
            writeImage(first, Color.RED);
            writeImage(second, Color.BLUE);
            InstrumentationRegistry.getInstrumentation().runOnMainSync(() ->
                ((MainActivity) activity).openAgentMedia(first.getAbsolutePath(), false,
                    Arrays.asList(new MainActivity.GalleryItem(first.getAbsolutePath(), false),
                        new MainActivity.GalleryItem(second.getAbsolutePath(), false))));
            awaitGalleryName("gallery-red.png");
            awaitCenterColor(Color.RED);
            SystemClock.sleep(300);
            Field imageDialog = MainActivity.class.getDeclaredField("imageDialog");
            imageDialog.setAccessible(true);
            Object preview = imageDialog.get(activity);
            assertNotNull(preview);
            swipeScreen(true);
            assertSame("Swiping must keep the same preview window", preview, imageDialog.get(activity));
            awaitGalleryName("gallery-blue.png");
            awaitCenterColor(Color.BLUE);
            captureScreen(context, "android-image-gallery-continuous.png");
            SystemClock.sleep(500);
            swipeScreen(false);
            assertSame(preview, imageDialog.get(activity));
            awaitCenterColor(Color.RED);
        } finally {
            InstrumentationRegistry.getInstrumentation().runOnMainSync(activity::finish);
            first.delete();
            second.delete();
        }
    }

    private static void awaitCenterColor(int expected) {
        long deadline = SystemClock.uptimeMillis() + 10000;
        while (SystemClock.uptimeMillis() < deadline) {
            Bitmap screen = InstrumentationRegistry.getInstrumentation()
                .getUiAutomation().takeScreenshot();
            if (screen != null) {
                int color = screen.getPixel(screen.getWidth() / 2, screen.getHeight() / 2);
                screen.recycle();
                if (expected == Color.RED && Color.red(color) > 180 && Color.blue(color) < 80)
                    return;
                if (expected == Color.BLUE && Color.blue(color) > 180 && Color.red(color) < 80)
                    return;
            }
            SystemClock.sleep(50);
        }
        throw new AssertionError("Preview did not show the expected image color");
    }

    @Test public void imagePreviewOffersIosActionsAndSavesToGallery() throws Exception {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        Activity activity = InstrumentationRegistry.getInstrumentation().startActivitySync(
            new Intent(context, MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        File image = AIAssistantController.shared(activity).workspaceFile("gallery-actions.png");
        assertNotNull(image);
        try {
            writeImage(image, Color.GREEN);
            int before = savedImageCount(context);
            InstrumentationRegistry.getInstrumentation().runOnMainSync(() ->
                ((MainActivity) activity).openAgentMedia(image.getAbsolutePath(), false,
                    Arrays.asList(new MainActivity.GalleryItem(image.getAbsolutePath(), false))));
            assertNotNull(awaitAccessibilityDescription("分享图片"));
            AccessibilityNodeInfo save = awaitAccessibilityDescription("保存图片");
            assertNotNull(save);
            long imageDeadline = SystemClock.uptimeMillis() + 10000;
            boolean presented = false;
            while (SystemClock.uptimeMillis() < imageDeadline) {
                Bitmap screen = InstrumentationRegistry.getInstrumentation()
                    .getUiAutomation().takeScreenshot();
                if (screen != null) {
                    int color = screen.getPixel(screen.getWidth() / 2, screen.getHeight() / 2);
                    presented = Color.green(color) > 150 && Color.red(color) < 100;
                    screen.recycle();
                    if (presented) break;
                }
                SystemClock.sleep(50);
            }
            assertTrue("Image preview did not present the decoded pixels", presented);
            captureScreen(context, "android-image-gallery-actions.png");
            assertTrue(save.performAction(AccessibilityNodeInfo.ACTION_CLICK));
            long deadline = SystemClock.uptimeMillis() + 10000;
            while (savedImageCount(context) <= before && SystemClock.uptimeMillis() < deadline)
                SystemClock.sleep(50);
            assertTrue("Image was not saved to the gallery", savedImageCount(context) > before);
        } finally {
            image.delete();
            InstrumentationRegistry.getInstrumentation().runOnMainSync(activity::finish);
        }
    }

    private static AccessibilityNodeInfo findAccessibilityDescription(
            AccessibilityNodeInfo node, String description) {
        if (node.getContentDescription() != null &&
            description.contentEquals(node.getContentDescription())) return node;
        for (int i = 0; i < node.getChildCount(); i++) {
            AccessibilityNodeInfo child = node.getChild(i);
            if (child == null) continue;
            AccessibilityNodeInfo found = findAccessibilityDescription(child, description);
            if (found != null) return found;
        }
        return null;
    }

    private static AccessibilityNodeInfo awaitAccessibilityDescription(String description) {
        long deadline = SystemClock.uptimeMillis() + 10000;
        while (SystemClock.uptimeMillis() < deadline) {
            AccessibilityNodeInfo root = InstrumentationRegistry.getInstrumentation()
                .getUiAutomation().getRootInActiveWindow();
            if (root != null) {
                AccessibilityNodeInfo found = findAccessibilityDescription(root, description);
                if (found != null) return found;
            }
            SystemClock.sleep(50);
        }
        return null;
    }

    private static int savedImageCount(Context context) {
        if (android.os.Build.VERSION.SDK_INT < 29) return 0;
        try (Cursor cursor = context.getContentResolver().query(
                MediaStore.Images.Media.getContentUri(MediaStore.VOLUME_EXTERNAL_PRIMARY),
                new String[]{MediaStore.Images.Media._ID},
                MediaStore.Images.Media.RELATIVE_PATH + "=?",
                new String[]{"Pictures/MaiChat/"}, null)) {
            return cursor == null ? 0 : cursor.getCount();
        }
    }

    private static void awaitGalleryName(String name) {
        long deadline = SystemClock.uptimeMillis() + 10000;
        while (SystemClock.uptimeMillis() < deadline) {
            AccessibilityNodeInfo root = InstrumentationRegistry.getInstrumentation()
                .getUiAutomation().getRootInActiveWindow();
            if (root != null && !root.findAccessibilityNodeInfosByText(name).isEmpty()) return;
            SystemClock.sleep(50);
        }
        throw new AssertionError("Gallery did not show " + name);
    }

    private static void swipeScreen(boolean next) {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        android.util.DisplayMetrics metrics = context.getResources().getDisplayMetrics();
        float from = metrics.widthPixels * (next ? 0.75f : 0.25f);
        float to = metrics.widthPixels * (next ? 0.25f : 0.75f);
        float y = metrics.heightPixels * 0.5f;
        long down = SystemClock.uptimeMillis();
        android.app.Instrumentation instrumentation = InstrumentationRegistry.getInstrumentation();
        instrumentation.sendPointerSync(MotionEvent.obtain(down, down,
            MotionEvent.ACTION_DOWN, from, y, 0));
        for (int step = 1; step <= 5; step++) {
            long time = SystemClock.uptimeMillis();
            instrumentation.sendPointerSync(MotionEvent.obtain(down, time,
                MotionEvent.ACTION_MOVE, from + (to - from) * step / 5, y, 0));
            SystemClock.sleep(25);
        }
        instrumentation.sendPointerSync(MotionEvent.obtain(down, SystemClock.uptimeMillis(),
            MotionEvent.ACTION_UP, to, y, 0));
    }

    @Test public void swipingActiveVideoMovesBetweenConversationFiles() throws Exception {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        Activity activity = InstrumentationRegistry.getInstrumentation().startActivitySync(
            new Intent(context, MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        File first = AIAssistantController.shared(activity).workspaceFile("swipe-first.mp4");
        File second = AIAssistantController.shared(activity).workspaceFile("swipe-second.mp4");
        assertNotNull(first);
        assertNotNull(second);
        try {
            for (File destination : Arrays.asList(first, second)) {
                try (InputStream input = InstrumentationRegistry.getInstrumentation()
                        .getContext().getAssets().open("gallery-sample.mp4");
                     FileOutputStream output = new FileOutputStream(destination)) {
                    byte[] buffer = new byte[8192];
                    int count;
                    while ((count = input.read(buffer)) >= 0) output.write(buffer, 0, count);
                }
            }
            AtomicBoolean opened = new AtomicBoolean();
            InstrumentationRegistry.getInstrumentation().runOnMainSync(() -> opened.set(
                ((MainActivity) activity).openAgentVideo(first.getAbsolutePath(),
                    Arrays.asList(first.getAbsolutePath(), second.getAbsolutePath()))));
            assertTrue(opened.get());
            awaitVideo(first.getAbsolutePath());
            swipeActiveVideo(true);
            awaitVideo(second.getAbsolutePath());
            swipeActiveVideo(false);
            awaitVideo(first.getAbsolutePath());
        } finally {
            InstrumentationRegistry.getInstrumentation().runOnMainSync(() -> {
                ((MainActivity) activity).commandAgentVideo("close", 0, 0, 0);
                activity.finish();
            });
            first.delete();
            second.delete();
        }
    }

    private static void awaitVideo(String expected) {
        long deadline = SystemClock.uptimeMillis() + 10000;
        while (SystemClock.uptimeMillis() < deadline) {
            MaiFfplayVideoView view = MaiFfplayVideoView.activeView();
            if (view != null && expected.equals(view.currentVideoPath())
                && view.hasPresentedFrame()) return;
            SystemClock.sleep(50);
        }
        assertEquals(expected, MaiFfplayVideoView.activeView() == null ? null
            : MaiFfplayVideoView.activeView().currentVideoPath());
        assertTrue(MaiFfplayVideoView.activeView().hasPresentedFrame());
    }

    private static void swipeActiveVideo(boolean next) {
        MaiFfplayVideoView view = MaiFfplayVideoView.activeView();
        assertNotNull(view);
        int[] position = new int[2];
        InstrumentationRegistry.getInstrumentation().runOnMainSync(
            () -> view.getLocationOnScreen(position));
        float y = position[1] + view.getHeight() * 0.45f;
        float from = position[0] + view.getWidth() * (next ? 0.8f : 0.2f);
        float to = position[0] + view.getWidth() * (next ? 0.2f : 0.8f);
        long down = SystemClock.uptimeMillis();
        android.app.Instrumentation instrumentation = InstrumentationRegistry.getInstrumentation();
        instrumentation.sendPointerSync(MotionEvent.obtain(down, down,
            MotionEvent.ACTION_DOWN, from, y, 0));
        for (int step = 1; step <= 5; step++) {
            long time = SystemClock.uptimeMillis();
            instrumentation.sendPointerSync(MotionEvent.obtain(down, time,
                MotionEvent.ACTION_MOVE, from + (to - from) * step / 5, y, 0));
            SystemClock.sleep(25);
        }
        instrumentation.sendPointerSync(MotionEvent.obtain(down, SystemClock.uptimeMillis(),
            MotionEvent.ACTION_UP, to, y, 0));
    }

    private static View findDescription(View root, String description) {
        if (root.getContentDescription() != null &&
            description.contentEquals(root.getContentDescription())) return root;
        if (root instanceof ViewGroup)
            for (int i = 0; i < ((ViewGroup) root).getChildCount(); i++) {
                View found = findDescription(((ViewGroup) root).getChildAt(i), description);
                if (found != null) return found;
            }
        return null;
    }

    private static void captureScreen(Context context, String name) throws Exception {
        Bitmap screenshot = InstrumentationRegistry.getInstrumentation()
            .getUiAutomation().takeScreenshot();
        assertNotNull(screenshot);
        File target = new File(context.getExternalFilesDir(null), name);
        try (FileOutputStream stream = new FileOutputStream(target)) {
            assertTrue(screenshot.compress(Bitmap.CompressFormat.PNG, 100, stream));
        } finally { screenshot.recycle(); }
    }

    @Test public void agentOpensControlsAndClosesGraphicsVideo() throws Exception {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        Activity activity = InstrumentationRegistry.getInstrumentation().startActivitySync(
            new Intent(context, MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        File video = AIAssistantController.shared(activity).workspaceFile("ffplay-test.mp4");
        assertNotNull(video);
        try {
            try (InputStream input = InstrumentationRegistry.getInstrumentation()
                    .getContext().getAssets().open("gallery-sample.mp4");
                 FileOutputStream output = new FileOutputStream(video)) {
                byte[] buffer = new byte[8192];
                int count;
                while ((count = input.read(buffer)) >= 0) output.write(buffer, 0, count);
            }
            MobileMediaTools tools = new MobileMediaTools((MainActivity) activity);
            JSONObject opened = tools.execute("maichat_play_video",
                new JSONObject().put("path", video.getName()));
            assertTrue(opened.getBoolean("opened"));
            long deadline = SystemClock.uptimeMillis() + 10000;
            MaiFfplayVideoView view;
            do {
                view = MaiFfplayVideoView.activeView();
                if (view != null && view.hasPresentedFrame()) break;
                SystemClock.sleep(50);
            } while (SystemClock.uptimeMillis() < deadline);
            assertNotNull("FFplay video view was not created", view);
            assertTrue("FFmpeg frame did not reach Graphics", view.hasPresentedFrame());
            MaiFfplayVideoView player = view;
            AtomicInteger dockHeight = new AtomicInteger();
            InstrumentationRegistry.getInstrumentation().runOnMainSync(() -> {
                View expand = findDescription(player.getRootView(), "放大视频");
                assertNotNull(expand);
                dockHeight.set(player.getHeight());
                assertTrue("Video should leave space for chat", dockHeight.get()
                    < activity.getWindow().getDecorView().getHeight());
                assertTrue(expand.performClick());
                assertSame(player, MaiFfplayVideoView.activeView());
            });
            InstrumentationRegistry.getInstrumentation().waitForIdleSync();
            assertTrue("Expand should enlarge the same player", player.getHeight() > dockHeight.get());
            InstrumentationRegistry.getInstrumentation().runOnMainSync(() -> {
                View collapse = findDescription(player.getRootView(), "返回分屏");
                assertNotNull(collapse);
                assertTrue(collapse.performClick());
                assertSame(player, MaiFfplayVideoView.activeView());
            });
            InstrumentationRegistry.getInstrumentation().getUiAutomation()
                .setRotation(UiAutomation.ROTATION_FREEZE_90);
            InstrumentationRegistry.getInstrumentation().waitForIdleSync();
            long landscapeDeadline = SystemClock.uptimeMillis() + 5000;
            while (activity.getResources().getConfiguration().orientation
                    != Configuration.ORIENTATION_LANDSCAPE
                    && SystemClock.uptimeMillis() < landscapeDeadline) SystemClock.sleep(50);
            assertEquals("Activity should reflow in landscape", Configuration.ORIENTATION_LANDSCAPE,
                activity.getResources().getConfiguration().orientation);
            assertTrue("Landscape dock should leave room beside the video",
                player.getWidth() < activity.getWindow().getDecorView().getWidth());
            captureScreen(context, "android-video-landscape-dock.png");
            InstrumentationRegistry.getInstrumentation().getUiAutomation()
                .setRotation(UiAutomation.ROTATION_FREEZE_0);
            InstrumentationRegistry.getInstrumentation().waitForIdleSync();
            InstrumentationRegistry.getInstrumentation().runOnMainSync(() -> {
                View speedButton = findDescription(player, "播放倍速");
                assertNotNull(speedButton);
                assertTrue(speedButton.performClick());
                View faster = findDescription(player, "播放倍速 1.5×");
                assertNotNull(faster);
                assertEquals(View.VISIBLE, faster.getVisibility());
            });
            SystemClock.sleep(1000);
            InstrumentationRegistry.getInstrumentation().runOnMainSync(() -> {
                View faster = findDescription(player, "播放倍速 1.5×");
                android.graphics.Rect bounds = new android.graphics.Rect();
                assertTrue(faster.getGlobalVisibleRect(bounds));
                assertTrue(bounds.width() > 0 && bounds.height() > 0);
            });
            captureScreen(context, "android-video-speed-menu.png");
            InstrumentationRegistry.getInstrumentation().runOnMainSync(() -> {
                View faster = findDescription(player, "播放倍速 1.5×");
                assertTrue(faster.performClick());
                assertEquals(View.GONE, faster.getParent() instanceof View
                    ? ((View) faster.getParent()).getVisibility() : View.VISIBLE);
            });
            SystemClock.sleep(300);
            captureScreen(context, "android-video-player-parity.png");
            assertEquals(true, tools.execute("maichat_video_command",
                new JSONObject().put("action", "pause")).getBoolean("accepted"));
            assertEquals(true, tools.execute("maichat_video_command",
                new JSONObject().put("action", "play")).getBoolean("accepted"));
            assertEquals(true, tools.execute("maichat_video_command",
                new JSONObject().put("action", "seek_percent").put("percent", 50))
                .getBoolean("accepted"));
            assertEquals(true, tools.execute("maichat_video_command",
                new JSONObject().put("action", "close")).getBoolean("accepted"));
            assertTrue(!activity.isFinishing());
        } finally {
            InstrumentationRegistry.getInstrumentation().getUiAutomation()
                .setRotation(UiAutomation.ROTATION_FREEZE_0);
            if (MaiFfplayVideoView.activeView() != null)
                InstrumentationRegistry.getInstrumentation().runOnMainSync(
                    () -> MaiFfplayVideoView.activeView().stop());
            video.delete();
            InstrumentationRegistry.getInstrumentation().runOnMainSync(activity::finish);
        }
    }
}
