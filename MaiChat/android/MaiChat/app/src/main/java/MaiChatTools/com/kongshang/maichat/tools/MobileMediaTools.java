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
            File file = AIAssistantController.shared(activity).workspaceFile(path);
            if (file == null || !file.isFile())
                throw new IllegalArgumentException("视频必须是 Agent 工作目录中的现有文件");
            if (!onUi(() -> activity.openAgentVideo(file.getCanonicalPath())))
                throw new IllegalStateException("MaiChat 无法打开视频弹窗");
            return new JSONObject().put("opened", true);
        }
        String action = arguments.optString("action", "").trim();
        if (action.isEmpty()) throw new IllegalArgumentException("缺少播放命令 action");
        double percent = arguments.optDouble("percent", -1);
        if (action.equals("seek_percent") && (percent < 0 || percent > 100))
            throw new IllegalArgumentException("percent 需要在 0 到 100 之间");
        if (!onUi(() -> activity.commandAgentVideo(action, percent)))
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
