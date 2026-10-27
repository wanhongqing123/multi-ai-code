package com.kongshang.maichat;

import static org.junit.Assert.assertTrue;
import static org.junit.Assert.assertFalse;

import android.content.Context;
import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Color;
import android.app.UiAutomation;
import android.accessibilityservice.AccessibilityService;
import android.provider.Settings;
import android.view.accessibility.AccessibilityNodeInfo;
import androidx.test.core.app.ActivityScenario;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import com.kongshang.maichat.tools.MaiChatHostTools;
import com.kongshang.maichat.tools.MobileScreenCaptureService;
import java.io.File;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;
import java.util.List;
import org.json.JSONObject;
import org.junit.Test;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
public final class MobileScreenCaptureInstrumentedTest {
    @Test public void capturesSettingsAfterSystemConsentAndFreezesOnReturn() throws Exception {
        Context app = InstrumentationRegistry.getInstrumentation().getTargetContext();
        CountDownLatch consent = new CountDownLatch(1);
        AtomicBoolean approved = new AtomicBoolean();
        AtomicReference<MainActivity> owner = new AtomicReference<>();
        try (ActivityScenario<MainActivity> scenario = ActivityScenario.launch(MainActivity.class)) {
            scenario.onActivity(activity -> {
                owner.set(activity);
                activity.startScreenShareForAgent(value -> {
                    approved.set(value);
                    consent.countDown();
                });
            });
            UiAutomation automation = InstrumentationRegistry.getInstrumentation()
                .getUiAutomation();
            assertTrue("system share scope was not shown",
                clickLabel(automation, "Share one app", 10_000));
            assertTrue("whole-screen choice was not shown",
                clickLabel(automation, "Share entire screen", 5_000));
            assertTrue("system approval button was not shown",
                clickLabel(automation, "Share screen", 5_000));
            assertTrue("system screen-sharing consent was not granted",
                consent.await(20, TimeUnit.SECONDS) && approved.get());
            assertTrue("foreground capture service did not start",
                MobileScreenCaptureService.awaitReady(15_000));
            Intent settings = new Intent(Settings.ACTION_SETTINGS)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            app.startActivity(settings);
            Thread.sleep(3_000);
            File beforeReturn = new File(app.getNoBackupFilesDir(),
                "screen-share-before-return.png");
            File beforeExport = new File(app.getFilesDir(),
                "screen-share-before-return.png");
            try {
                MobileScreenCaptureService.copyLatestFrame(beforeReturn);
                Files.copy(beforeReturn.toPath(), beforeExport.toPath(),
                    StandardCopyOption.REPLACE_EXISTING);
            } finally { beforeReturn.delete(); }
            assertTrue(automation.performGlobalAction(AccessibilityService.GLOBAL_ACTION_BACK));
            Thread.sleep(1_000);
            scenario.onActivity(owner::set);
            File output = new File(app.getNoBackupFilesDir(), "screen-share-integration.png");
            File toolOutput = new File(app.getNoBackupFilesDir(),
                "screenshots/mobile-screen-host-test.png");
            try {
                long age = MobileScreenCaptureService.copyLatestFrame(output);
                assertTrue("captured frame should be recent", age < 10_000);
                BitmapFactory.Options bounds = new BitmapFactory.Options();
                bounds.inJustDecodeBounds = true;
                BitmapFactory.decodeFile(output.getPath(), bounds);
                assertTrue(bounds.outWidth > 0 && bounds.outHeight > 0);
                File exported = new File(app.getFilesDir(),
                    "screen-share-integration.png");
                Files.copy(output.toPath(), exported.toPath(),
                    StandardCopyOption.REPLACE_EXISTING);
                assertTrue("return to MaiChat replaced the target screen with a blank frame",
                    matchingContentFraction(beforeExport, exported) > 0.85);
                MaiChatHostTools host = new MaiChatHostTools(owner.get(), null);
                File oldLocation = new File(app.getFilesDir().getParentFile(),
                    "screenshots/old-agent-location.png");
                boolean rejectedOldLocation = false;
                try {
                    host.execute("mobile_capture_screen", new JSONObject()
                        .put("output_path", oldLocation.getCanonicalPath()));
                } catch (IllegalArgumentException expected) {
                    rejectedOldLocation = true;
                }
                assertTrue("old Agent path must stay outside the no_backup write scope",
                    rejectedOldLocation);
                JSONObject hostResult = (JSONObject) host.execute(
                    "mobile_capture_screen", new JSONObject()
                        .put("output_path", toolOutput.getCanonicalPath()));
                assertTrue("Agent host did not accept its no_backup screenshot path",
                    hostResult.getBoolean("captured"));
                assertTrue("Agent host image differs from the target app",
                    matchingContentFraction(beforeExport, toolOutput) > 0.85);
            } finally {
                output.delete();
                toolOutput.delete();
                MobileScreenCaptureService.stop(app);
            }
            long deadline = android.os.SystemClock.elapsedRealtime() + 3_000;
            while (MobileScreenCaptureService.isCapturing() &&
                   android.os.SystemClock.elapsedRealtime() < deadline) Thread.sleep(50);
            assertFalse("stopped sharing must clear the capture session",
                MobileScreenCaptureService.isCapturing());
        }
    }

    private static double matchingContentFraction(File first, File second) {
        Bitmap before = BitmapFactory.decodeFile(first.getPath());
        Bitmap after = BitmapFactory.decodeFile(second.getPath());
        if (before == null || after == null) return 0;
        try {
            if (before.getWidth() != after.getWidth() ||
                before.getHeight() != after.getHeight()) return 0;
            int close = 0;
            int total = 0;
            int xStep = Math.max(1, before.getWidth() / 40);
            int yStep = Math.max(1, before.getHeight() / 40);
            for (int y = before.getHeight() / 10;
                 y < before.getHeight() * 9 / 10; y += yStep) {
                for (int x = before.getWidth() / 10;
                     x < before.getWidth() * 9 / 10; x += xStep) {
                    int oldPixel = before.getPixel(x, y);
                    int newPixel = after.getPixel(x, y);
                    if (Math.abs(Color.red(oldPixel) - Color.red(newPixel)) <= 30 &&
                        Math.abs(Color.green(oldPixel) - Color.green(newPixel)) <= 30 &&
                        Math.abs(Color.blue(oldPixel) - Color.blue(newPixel)) <= 30) close++;
                    total++;
                }
            }
            return total == 0 ? 0 : (double) close / total;
        } finally {
            before.recycle();
            after.recycle();
        }
    }

    private static boolean clickLabel(UiAutomation automation, String label,
                                      long timeoutMs) throws InterruptedException {
        long deadline = android.os.SystemClock.elapsedRealtime() + timeoutMs;
        while (android.os.SystemClock.elapsedRealtime() < deadline) {
            AccessibilityNodeInfo root = automation.getRootInActiveWindow();
            if (root != null) {
                List<AccessibilityNodeInfo> matches = root.findAccessibilityNodeInfosByText(label);
                for (AccessibilityNodeInfo match : matches) {
                    if (match.getText() == null || !label.contentEquals(match.getText())) continue;
                    for (AccessibilityNodeInfo node = match; node != null; node = node.getParent()) {
                        if (node.isClickable() &&
                            node.performAction(AccessibilityNodeInfo.ACTION_CLICK)) return true;
                    }
                }
            }
            Thread.sleep(100);
        }
        return false;
    }
}
