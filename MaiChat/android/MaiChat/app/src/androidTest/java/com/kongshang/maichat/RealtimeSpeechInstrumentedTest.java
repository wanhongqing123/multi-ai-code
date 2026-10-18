// 文件作用：Android 设备集成测试；RealtimeSpeechInstrumentedTest 负责覆盖对应界面或服务的设备行为，本文件给出其验证。
// 运行边界：仅供自动化验证使用，不作为正式 App 或 Agent 的用户功能入口。
// 覆盖场景：emulatorRecordingStopsOffUiAndProducesCompleteAacFrames、
// syntheticSpeechGetsPartialAndFinalResultsFromTheConfiguredService。
package com.kongshang.maichat;

import androidx.test.platform.app.InstrumentationRegistry;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import org.junit.Test;
import org.junit.Assume;
import org.junit.runner.RunWith;
import java.io.File;
import java.io.IOException;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import static org.junit.Assert.*;

@RunWith(AndroidJUnit4.class)
public class RealtimeSpeechInstrumentedTest {
    @Test public void emulatorRecordingStopsOffUiAndProducesCompleteAacFrames() throws Exception {
        Assume.assumeTrue("Synthetic emulator microphone only", android.os.Build.MODEL.startsWith("sdk_"));
        android.content.Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        InstrumentationRegistry.getInstrumentation().getUiAutomation().grantRuntimePermission(
                context.getPackageName(), android.Manifest.permission.RECORD_AUDIO);
        try (androidx.test.core.app.ActivityScenario<MainActivity> scenario =
                        androidx.test.core.app.ActivityScenario.launch(MainActivity.class)) {
            CountDownLatch done = new CountDownLatch(1), uiPulse = new CountDownLatch(1);
            AtomicReference<File> output = new AtomicReference<>();
            VoiceRecordingController recorder =
                    new VoiceRecordingController(new RemoteIMMediaStore(RemoteIMMediaPaths.forApp(context)));
            try {
                long startedAt = android.os.SystemClock.elapsedRealtime();
                scenario.onActivity(activity -> {
                    recorder.start(new VoiceRecordingController.Completion() {
                        @Override public void finished(File file, int seconds) { output.set(file); done.countDown(); }
                        @Override public void failed() { done.countDown(); }
                    });
                    new android.os.Handler(android.os.Looper.getMainLooper()).post(uiPulse::countDown);
                });
                assertTrue("Recorder initialization must not block UI", uiPulse.await(500, TimeUnit.MILLISECONDS));
                assertTrue("UI was blocked before the heartbeat could run",
                        android.os.SystemClock.elapsedRealtime() - startedAt < 500);
                Thread.sleep(1400);
                scenario.onActivity(activity -> recorder.finish(false));
                assertTrue("Recorder did not stop", done.await(12, TimeUnit.SECONDS));
                assertNotNull("Emulator recorder failed", output.get());
                try (AdtsFrameReader frames = new AdtsFrameReader(output.get())) {
                    assertTrue(frames.readUntilSample(Long.MAX_VALUE).length > 0);
                    assertTrue("Native recorder must finish the final AAC frame", frames.atEnd());
                    assertTrue(frames.samples() >= 8000);
                }
            } finally {
                recorder.close();
                if (output.get() != null) output.get().delete();
            }
        }
    }

    @Test public void releaseBeforeAsrTicketDoesNotLoseTheRecordingOrBlockUi() throws Exception {
        Assume.assumeTrue("Synthetic emulator microphone only", android.os.Build.MODEL.startsWith("sdk_"));
        android.content.Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        InstrumentationRegistry.getInstrumentation().getUiAutomation().grantRuntimePermission(
            context.getPackageName(), android.Manifest.permission.RECORD_AUDIO);
        CountDownLatch requested = new CountDownLatch(1), issue = new CountDownLatch(1);
        CountDownLatch failed = new CountDownLatch(1);
        AtomicReference<File> recording = new AtomicReference<>();
        RemoteIMMediaStore store = new RemoteIMMediaStore(RemoteIMMediaPaths.forApp(context));
        VoiceRecordingController recorder = new VoiceRecordingController(store);
        try {
            boolean started = recorder.tryStart(new VoiceRecordingController.Completion() {
                @Override public void finished(File file, int seconds) {
                    recording.set(file); failed.countDown();
                }
                @Override public void failed() { failed.countDown(); }
            }, () -> {
                requested.countDown();
                if (!issue.await(10, TimeUnit.SECONDS)) throw new IOException("ticket timed out");
                return null;
            });
            assertTrue(started);
            assertTrue("Ticket was not requested while recording", requested.await(5, TimeUnit.SECONDS));
            Thread.sleep(1200);
            recorder.finish(false);
            assertFalse("Recorder must wait for the ticket result", failed.await(300, TimeUnit.MILLISECONDS));
            issue.countDown();
            assertTrue("Ticket failure did not reach the UI", failed.await(5, TimeUnit.SECONDS));
            assertNull("Failed ASR must not deliver a voice file", recording.get());
        } finally {
            issue.countDown();
            recorder.close();
            if (recording.get() != null) recording.get().delete();
        }
    }

}
