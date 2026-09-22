package com.kongshang.maichat;

import androidx.test.platform.app.InstrumentationRegistry;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import org.junit.Test;
import org.junit.Assume;
import org.junit.runner.RunWith;
import java.io.File;
import java.util.List;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import static org.junit.Assert.*;

@RunWith(AndroidJUnit4.class)
public class RealtimeSpeechInstrumentedTest {
    @Test public void emulatorRecordingStopsOffUiAndProducesCompleteAacFrames() throws Exception {
        Assume.assumeTrue("Synthetic emulator microphone only", android.os.Build.MODEL.startsWith("sdk_"));
        android.content.Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        InstrumentationRegistry.getInstrumentation().getUiAutomation().grantRuntimePermission(context.getPackageName(), android.Manifest.permission.RECORD_AUDIO);
        try (androidx.test.core.app.ActivityScenario<MainActivity> scenario = androidx.test.core.app.ActivityScenario.launch(MainActivity.class)) {
            CountDownLatch done = new CountDownLatch(1), uiPulse = new CountDownLatch(1);
            AtomicReference<File> output = new AtomicReference<>();
            VoiceRecordingController recorder = new VoiceRecordingController(new RemoteIMMediaStore(RemoteIMMediaPaths.forApp(context)));
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
                assertTrue("UI was blocked before the heartbeat could run", android.os.SystemClock.elapsedRealtime() - startedAt < 500);
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

    @Test public void syntheticSpeechGetsPartialAndFinalResultsFromTheConfiguredService() throws Exception {
        File audio = new File(InstrumentationRegistry.getInstrumentation().getTargetContext().getCacheDir(), "speech-synthetic.aac");
        Assume.assumeTrue("Requires a local synthetic fixture and development credentials", audio.isFile() && !BuildConfig.TENCENT_ASR_APP_ID.isEmpty());
        CountDownLatch done = new CountDownLatch(1);
        AtomicReference<String> result = new AtomicReference<>();
        List<String> events = new CopyOnWriteArrayList<>();
        List<String> partials = new CopyOnWriteArrayList<>();
        TencentRealtimeSpeechStream stream = new TencentRealtimeSpeechStream(audio,
            BuildConfig.TENCENT_ASR_APP_ID, BuildConfig.TENCENT_ASR_SECRET_ID, BuildConfig.TENCENT_ASR_SECRET_KEY,
            new TencentRealtimeSpeechStream.Listener() {
                @Override public void partial(String text) { partials.add(text); }
                @Override public void diagnostic(String event) { events.add(event); }
                @Override public void finished(String text) { result.set(text); done.countDown(); }
            });
        try {
            stream.endAudio();
            assertTrue("ASR request did not finish", done.await(25, TimeUnit.SECONDS));
            assertNotNull("ASR failed: " + events, result.get());
            assertTrue("No streamed partial result", !partials.isEmpty());
            assertTrue("Expected recognized synthetic phrase", result.get().contains("测试"));
            assertTrue("Final result must come from the server: " + events, events.stream().anyMatch(value -> value.startsWith("asr-server-final")));
        } finally { stream.cancel(); }
    }
}
