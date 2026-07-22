package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;

import org.junit.Test;

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;

public class LocalChatHistoryStoreTest {
    @Test
    public void savesAndRestoresContactsAndMessages() throws Exception {
        Path root = Files.createTempDirectory("maichat-android-history");
        LocalChatHistoryStore store = new LocalChatHistoryStore(root.toFile());
        ChatState state = new ChatState("android-user");
        state.upsertContact(new RemoteIMContact("mac-office", "Mac Office"));
        state.selectPeer("mac-office");
        RemoteIMMessage text = state.queueOutgoingText("检查构建");
        state.updateMessageStatus(text.id(), RemoteIMMessage.Status.SENT);
        state.queueOutgoingImage("/tmp/photo.png", 640, 480, 4096);
        state.receiveVoice("/tmp/reply.m4a", 4, "mac-office");
        state.receiveFile("/tmp/report.html", "mac-office", "report.html", "text/html", 2048);

        store.save(state);

        ChatState restored = store.load("android-user");
        assertEquals(state.contacts(), restored.contacts());
        assertEquals(4, restored.messagesWith("mac-office").size());
        assertEquals(RemoteIMMessage.Status.SENT, restored.messagesWith("mac-office").get(0).status());
        assertNotNull(restored.messagesWith("mac-office").get(1).imageAttachment());
        assertNotNull(restored.messagesWith("mac-office").get(2).voiceAttachment());
        assertNotNull(restored.messagesWith("mac-office").get(3).fileAttachment());
        assertEquals("report.html", restored.messagesWith("mac-office").get(3).fileAttachment().fileName());
    }

    @Test
    public void returnsEmptyStateWhenHistoryDoesNotExist() throws Exception {
        Path root = Files.createTempDirectory("maichat-android-history-empty");
        LocalChatHistoryStore store = new LocalChatHistoryStore(root.toFile());

        ChatState restored = store.load("android-user");

        assertEquals(List.of(), restored.contacts());
        assertEquals(List.of(), restored.messages());
    }
}
