package com.kongshang.maichat.tools;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.os.SystemClock;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import com.kongshang.maichat.AIAssistantController;
import com.kongshang.maichat.MainActivity;
import com.kongshang.maichat.MaiFfplayVideoView;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import org.json.JSONObject;
import org.junit.Test;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
public final class MaiFfplayPlaybackInstrumentedTest {
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
            if (MaiFfplayVideoView.activeView() != null)
                InstrumentationRegistry.getInstrumentation().runOnMainSync(
                    () -> MaiFfplayVideoView.activeView().stop());
            video.delete();
            InstrumentationRegistry.getInstrumentation().runOnMainSync(activity::finish);
        }
    }
}
