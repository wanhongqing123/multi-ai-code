// 文件作用：Android 本地测试；AIAssistantMediaMentionsTest 负责覆盖对应类的状态和数据行为，本文件给出其验证。
// 运行边界：仅供自动化验证使用，不作为正式 App 或 Agent 的用户功能入口。
// 覆盖场景：smallMessagesGetCardsAndFileListsStayText。
package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

import java.io.File;
import java.nio.file.Files;
import java.util.List;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

public class AIAssistantMediaMentionsTest {
    @Rule public TemporaryFolder folder = new TemporaryFolder();

    @Test public void smallMessagesGetCardsAndFileListsStayText() throws Exception {
        File root = folder.newFolder("Workspace");
        for (String name : new String[] {"clip.mp4", "voice.wav", "with space.mov",
                "third.mp4", "fourth.mp4", "fifth.mp4"})
            Files.write(new File(root, name).toPath(), new byte[] {1, 2, 3});
        List<AIAssistantMediaMentions.Mention> cards = AIAssistantMediaMentions.cards(
            "视频 `clip.mp4`，音频 voice.wav，还有 `with space.mov`。missing.mp4",
            name -> new File(root, name));
        assertEquals(3, cards.size());
        assertEquals("clip.mp4", cards.get(0).file.getName());
        assertEquals("with space.mov", cards.get(1).file.getName());
        assertEquals("voice.wav", cards.get(2).file.getName());
        assertTrue(AIAssistantMediaMentions.cards(
            "clip.mp4 voice.wav third.mp4 fourth.mp4 fifth.mp4",
            name -> new File(root, name)).isEmpty());
        assertTrue(AIAssistantMediaMentions.cards(
            "```\nclip.mp4\n```", name -> new File(root, name)).isEmpty());
        assertTrue(AIAssistantMediaMentions.cards(
            "`const file = \"clip.mp4\"`", name -> new File(root, name)).isEmpty());
    }
}
