package com.kongshang.maichat.tools;

import android.os.Looper;
import com.kongshang.maichat.AIAssistantController;
import com.kongshang.maichat.MainActivity;
import java.io.File;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import org.json.JSONObject;

/** Agent commands operate the same ffplay popup used by IM video messages. */
final class MobileMediaTools {
    private final MainActivity activity;

    MobileMediaTools(MainActivity activity) { this.activity = activity; }

    JSONObject execute(String tool, JSONObject arguments) throws Exception {
        if (tool.equals("maichat_play_video")) {
            String path = arguments.optString("path", "").trim();
            double startSeconds = arguments.optDouble("start_seconds", 0);
            if (!Double.isFinite(startSeconds) || startSeconds < 0 ||
                startSeconds > Long.MAX_VALUE / 1_000_000.0)
                throw new IllegalArgumentException("start_seconds 必须是非负的原片秒数");
            File file = AIAssistantController.shared(activity).workspaceFile(path);
            if (file == null || !file.isFile())
                throw new IllegalArgumentException("视频必须是 Agent 工作目录中的现有文件");
            long startPositionUs = Math.round(startSeconds * 1_000_000);
            if (!onUi(() -> activity.openAgentVideo(file.getCanonicalPath(), startPositionUs)))
                throw new IllegalStateException("MaiChat 无法打开视频弹窗");
            return new JSONObject().put("opened", true).put("start_seconds", startSeconds);
        }
        String action = arguments.optString("action", "").trim();
        if (action.isEmpty()) throw new IllegalArgumentException("缺少播放命令 action");
        double percent = arguments.optDouble("percent", -1);
        double speed = arguments.optDouble("speed", -1);
        double seconds = arguments.optDouble("seconds", -1);
        if (action.equals("seek_percent") && (percent < 0 || percent > 100))
            throw new IllegalArgumentException("percent 需要在 0 到 100 之间");
        if (action.equals("seek_seconds") && (!Double.isFinite(seconds) || seconds < 0))
            throw new IllegalArgumentException("seconds 必须是非负的源视频秒数");
        if (action.equals("set_speed") && speed != 0.25 && speed != 0.5 &&
            speed != 1.0 && speed != 1.5 && speed != 2.0)
            throw new IllegalArgumentException("speed 只支持 0.25、0.5、1、1.5、2");
        if (!onUi(() -> activity.commandAgentVideo(action, percent, speed, seconds)))
            throw new IllegalStateException("播放命令不可用或没有打开的视频");
        return new JSONObject().put("accepted", true);
    }

    private boolean onUi(java.util.concurrent.Callable<Boolean> action) throws Exception {
        if (Looper.myLooper() == Looper.getMainLooper()) return action.call();
        CountDownLatch finished = new CountDownLatch(1);
        AtomicBoolean result = new AtomicBoolean();
        activity.runOnUiThread(() -> {
            try { result.set(action.call()); }
            catch (Exception ignored) { result.set(false); }
            finally { finished.countDown(); }
        });
        if (!finished.await(15, TimeUnit.SECONDS))
            throw new IllegalStateException("等待 MaiChat 播放窗口超时");
        return result.get();
    }
}
