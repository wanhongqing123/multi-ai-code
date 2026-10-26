// Agent screen-sharing bridge. The OS consent UI owns the capture session.
package com.kongshang.maichat.tools;

import android.os.Looper;
import com.kongshang.maichat.MainActivity;
import java.io.File;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import org.json.JSONObject;

final class MobileScreenCaptureTools {
    private final MainActivity activity;

    MobileScreenCaptureTools(MainActivity activity) { this.activity = activity; }

    JSONObject execute(String tool, JSONObject arguments) throws Exception {
        if (tool.equals("mobile_stop_screen_share")) {
            MobileScreenCaptureService.stop(activity);
            return new JSONObject().put("sharing", false);
        }
        if (tool.equals("mobile_start_screen_share")) {
            if (Looper.myLooper() == Looper.getMainLooper())
                throw new IllegalStateException("屏幕共享授权不能阻塞 UI 线程");
            CountDownLatch finished = new CountDownLatch(1);
            AtomicBoolean approved = new AtomicBoolean();
            activity.runOnUiThread(() -> activity.startScreenShareForAgent(value -> {
                approved.set(value);
                finished.countDown();
            }));
            if (!finished.await(120, TimeUnit.SECONDS))
                throw new IllegalStateException("等待屏幕共享授权超时");
            if (!approved.get())
                throw new IllegalStateException("用户取消了系统屏幕共享授权");
            if (!MobileScreenCaptureService.awaitReady(15_000))
                throw new IllegalStateException("系统已授权，但屏幕共享未能启动");
            return new JSONObject().put("sharing", true)
                .put("next_step", "Switch to the target app, then return to MaiChat and ask me to capture its screen.");
        }
        if (!tool.equals("mobile_capture_screen"))
            throw new IllegalArgumentException("未知屏幕共享工具");
        String destination = arguments.optString("output_path", "");
        File output = new File(destination).getCanonicalFile();
        File files = activity.getNoBackupFilesDir().getCanonicalFile();
        if (!output.getPath().startsWith(files.getPath() + File.separator) ||
            !output.getName().endsWith(".png"))
            throw new IllegalArgumentException("截图目标必须位于 App 缓存目录");
        long ageMs = MobileScreenCaptureService.copyLatestFrame(output);
        return new JSONObject().put("captured", true).put("age_ms", ageMs)
            .put("note", "Captured the latest frame from the system screen-sharing session.");
    }
}
