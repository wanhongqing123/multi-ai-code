// 文件作用：验证 Android 创作中心工具调用按消息汇总的状态与数量。
package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertArrayEquals;

import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.Test;

public final class AIAssistantToolSummaryTest {
    @Test public void groupsToolCallsIntoOneStatusLine() throws Exception {
        JSONArray parts = new JSONArray()
            .put(new JSONObject().put("kind", "text").put("text", "hello"))
            .put(new JSONObject().put("kind", "tool").put("tool", "ffmpeg")
                .put("state", "completed"))
            .put(new JSONObject().put("kind", "tool").put("tool", "ffmpeg")
                .put("state", "completed"))
            .put(new JSONObject().put("kind", "tool").put("tool", "agent_send_media")
                .put("state", "completed"));
        assertEquals("工具调用 3 次，已完成 · ffmpeg ×2 · agent_send_media ×1",
            AIAssistantPanel.toolGroupTitle(parts));
        parts.getJSONObject(2).put("state", "running");
        assertEquals("工具调用 3 次，运行中 · ffmpeg ×2 · agent_send_media ×1",
            AIAssistantPanel.toolGroupTitle(parts));
        parts.getJSONObject(2).put("state", "error");
        assertEquals("工具调用 3 次，有失败 · ffmpeg ×2 · agent_send_media ×1",
            AIAssistantPanel.toolGroupTitle(parts));
        JSONObject delivered = new JSONObject().put("kind", "tool")
            .put("tool", "agent_send_media").put("state", "completed")
            .put("output", new JSONObject().put("delivery", "current_ai_session")
                .put("path", "result.mp4").put("type", "video")
                .put("mime_type", "video/mp4").toString());
        parts.put(delivered);
        assertEquals("工具调用 3 次，有失败 · ffmpeg ×2 · agent_send_media ×1",
            AIAssistantPanel.toolGroupTitle(parts));
        assertEquals("", AIAssistantPanel.toolGroupTitle(new JSONArray().put(delivered)));
    }

    @Test public void videoCardsPreserveLandscapeAndPortraitAspectRatios() {
        assertArrayEquals(new int[] {280, 181}, AIAssistantPanel.videoCardSize(0, 0));
        assertArrayEquals(new int[] {280, 158}, AIAssistantPanel.videoCardSize(1920, 1080));
        assertArrayEquals(new int[] {180, 320}, AIAssistantPanel.videoCardSize(1080, 1920));
    }

    @Test public void quotedMessagesKeepMediaAndReadableText() throws Exception {
        JSONObject message = new JSONObject().put("parts", new JSONArray()
            .put(new JSONObject().put("kind", "image"))
            .put(new JSONObject().put("kind", "video"))
            .put(new JSONObject().put("kind", "text").put("text", "  Hello 🙂  ")));
        assertEquals("[图片、视频] Hello 🙂", AIAssistantPanel.quotePreview(message));
    }
}
