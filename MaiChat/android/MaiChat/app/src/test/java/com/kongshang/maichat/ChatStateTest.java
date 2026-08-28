package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

import java.util.List;

public class ChatStateTest {
    @Test
    public void contactGroupsRenameDeleteAndPreserveProfileAssignments() {
        ChatState state = new ChatState("android-user");
        assertTrue(state.addContactGroup(" 同事 "));
        assertTrue(state.addContactGroup("未分组"));
        assertFalse(state.addContactGroup("同事"));
        assertFalse(state.addContactGroup("  "));

        state.upsertContact(new RemoteIMContact("mac-office", "Mac Office"));
        assertTrue(state.setContactGroup("mac-office", "同事"));
        state.upsertContact(new RemoteIMContact(
            "mac-office", "办公室电脑", "https://example.com/avatar.png"
        ));
        assertEquals("同事", state.contacts().get(0).groupName());
        assertEquals("办公室电脑", state.contacts().get(0).displayName());

        assertTrue(state.renameContactGroup("同事", "工作"));
        assertEquals("工作", state.contacts().get(0).groupName());
        assertFalse(state.renameContactGroup("工作", "未分组"));
        assertTrue(state.removeContactGroup("工作"));
        assertEquals("", state.contacts().get(0).groupName());
        assertEquals(List.of("未分组"), state.contactGroups());
    }

    @Test
    public void unknownContactGroupSelfHealsToUngrouped() {
        ChatState state = new ChatState("android-user");
        state.setContactGroups(List.of("同事"));
        state.upsertContact(new RemoteIMContact("mac-office", "Mac", "", "幽灵组"));

        assertEquals("", state.contacts().get(0).groupName());
        assertTrue(state.setContactGroup("mac-office", "幽灵组"));
        assertEquals("", state.contacts().get(0).groupName());
    }

    @Test
    public void upsertsContactAndQueuesOutgoingText() {
        ChatState state = new ChatState("android-user");

        state.upsertContact(new RemoteIMContact("mac-office", "Mac Office"));
        state.selectPeer("mac-office");
        RemoteIMMessage message = state.queueOutgoingText("检查构建");

        assertEquals("mac-office", state.selectedPeerId());
        assertEquals(1, state.contacts().size());
        assertEquals("检查构建", message.text());
        assertEquals(RemoteIMMessage.Direction.OUTGOING, message.direction());
        assertEquals(RemoteIMMessage.Status.PENDING, message.status());
        assertEquals(List.of(message), state.messagesWith("mac-office"));
    }

    @Test
    public void recordsIncomingTextAndAutoAddsContact() {
        ChatState state = new ChatState("android-user");

        RemoteIMMessage message = state.receiveText("处理完成", "mac-office");

        assertEquals("处理完成", message.text());
        assertEquals(RemoteIMMessage.Direction.INCOMING, message.direction());
        assertEquals(RemoteIMMessage.Status.RECEIVED, message.status());
        assertEquals("mac-office", state.contacts().get(0).userId());
    }

    @Test
    public void queuesOutgoingImageWithAttachment() {
        ChatState state = new ChatState("android-user");
        state.upsertContact(new RemoteIMContact("mac-office", "Mac Office"));
        state.selectPeer("mac-office");

        RemoteIMMessage message = state.queueOutgoingImage("/tmp/photo.png", 640, 480, 4096);

        assertEquals("[图片消息] photo.png", message.text());
        assertNotNull(message.imageAttachment());
        assertEquals("/tmp/photo.png", message.imageAttachment().localPath());
        assertEquals(640, message.imageAttachment().width());
        assertEquals(480, message.imageAttachment().height());
        assertEquals(4096, message.imageAttachment().sizeBytes());
    }

    @Test
    public void queuesAndReceivesVoiceMessages() {
        ChatState state = new ChatState("android-user");
        state.upsertContact(new RemoteIMContact("mac-office", "Mac Office"));
        state.selectPeer("mac-office");

        RemoteIMMessage outgoing = state.queueOutgoingVoice("/tmp/out.m4a", 3);
        RemoteIMMessage incoming = state.receiveVoice("/tmp/in.m4a", 4, "mac-office");

        assertEquals("[语音消息 3s]", outgoing.text());
        assertEquals(RemoteIMMessage.Direction.OUTGOING, outgoing.direction());
        assertNotNull(outgoing.voiceAttachment());
        assertEquals("[语音消息 4s]", incoming.text());
        assertEquals(RemoteIMMessage.Direction.INCOMING, incoming.direction());
        assertNotNull(incoming.voiceAttachment());
    }

    @Test
    public void receivesMarkdownFileWithAttachment() {
        ChatState state = new ChatState("android-user");

        RemoteIMMessage message = state.receiveFile(
            "/tmp/remote-im/report.md",
            "mac-office",
            "report.md",
            "text/markdown",
            4096
        );

        assertEquals("[文件消息] report.md", message.text());
        assertEquals(RemoteIMMessage.Direction.INCOMING, message.direction());
        assertEquals(RemoteIMMessage.Status.RECEIVED, message.status());
        assertNotNull(message.fileAttachment());
        assertEquals("/tmp/remote-im/report.md", message.fileAttachment().localPath());
        assertEquals("report.md", message.fileAttachment().fileName());
        assertEquals("text/markdown", message.fileAttachment().mimeType());
        assertEquals(4096, message.fileAttachment().sizeBytes());
    }

    @Test
    public void messageStatusCanBeUpdated() {
        ChatState state = new ChatState("android-user");
        state.upsertContact(new RemoteIMContact("mac-office", "Mac Office"));
        state.selectPeer("mac-office");
        RemoteIMMessage message = state.queueOutgoingText("ping");

        assertTrue(state.updateMessageStatus(message.id(), RemoteIMMessage.Status.SENT));

        assertEquals(RemoteIMMessage.Status.SENT, state.messagesWith("mac-office").get(0).status());
        assertFalse(state.updateMessageStatus("missing", RemoteIMMessage.Status.SENT));
    }

    @Test
    public void deduplicatesIncomingMessagesByRemoteId() {
        ChatState state = new ChatState("android-user");

        RemoteIMMessage first = state.receiveText(
            "完成",
            "mac-office",
            "remote-1",
            100L,
            RemoteIMOrigin.MACHINE
        );
        RemoteIMMessage duplicate = state.receiveText(
            "重复",
            "mac-office",
            "remote-1",
            200L,
            RemoteIMOrigin.MACHINE
        );

        assertEquals(first, duplicate);
        assertEquals(1, state.messagesWith("mac-office").size());
        assertEquals(RemoteIMOrigin.MACHINE, first.origin());
    }

    @Test
    public void deletingAContactAlsoRemovesOnlyThatConversation() {
        ChatState state = new ChatState("android-user");
        state.receiveText("A", "mac-office");
        state.receiveText("B", "house-office");

        assertTrue(state.removeContact("mac-office"));

        assertTrue(state.messagesWith("mac-office").isEmpty());
        assertEquals(1, state.messagesWith("house-office").size());
    }

    @Test
    public void returnsPeerMessagesChronologically() {
        ChatState state = new ChatState("android-user");
        RemoteIMMessage newest = new RemoteIMMessage(
            "newest",
            "android-user",
            "mac-office",
            "最新消息",
            RemoteIMMessage.Direction.OUTGOING,
            RemoteIMMessage.Status.SENT,
            300L,
            null,
            null
        );
        RemoteIMMessage oldest = new RemoteIMMessage(
            "oldest",
            "mac-office",
            "android-user",
            "最早消息",
            RemoteIMMessage.Direction.INCOMING,
            RemoteIMMessage.Status.RECEIVED,
            100L,
            null,
            null
        );
        RemoteIMMessage middle = new RemoteIMMessage(
            "middle",
            "mac-office",
            "android-user",
            "中间消息",
            RemoteIMMessage.Direction.INCOMING,
            RemoteIMMessage.Status.RECEIVED,
            200L,
            null,
            null
        );
        state.addRestoredMessage(newest);
        state.addRestoredMessage(oldest);
        state.addRestoredMessage(middle);

        assertEquals(List.of(oldest, middle, newest), state.messagesWith("mac-office"));
    }

    @Test
    public void receivesVideoAndDedupesByRemoteId() {
        ChatState state = new ChatState("android-user");
        state.upsertContact(new RemoteIMContact("mac-office", "Mac Office"));

        RemoteIMMessage message = state.receiveVideo(
            "/tmp/clip.mp4",
            "/tmp/clip-cover.jpg",
            12,
            1080,
            1920,
            4_194_304L,
            "mac-office",
            "sdk-video-1",
            1_700_000_000_000L,
            RemoteIMOrigin.HUMAN
        );

        assertEquals("[视频消息 12s]", message.text());
        assertEquals(RemoteIMMessage.Direction.INCOMING, message.direction());
        assertNotNull(message.videoAttachment());
        assertEquals("/tmp/clip-cover.jpg", message.videoAttachment().coverPath());

        // 同一条 remoteId 再来一次不能变成两条：封面与视频是两次下载，
        // 上层对同一条消息可能不止投递一次。
        RemoteIMMessage again = state.receiveVideo(
            "/tmp/clip.mp4",
            "/tmp/clip-cover.jpg",
            12,
            1080,
            1920,
            4_194_304L,
            "mac-office",
            "sdk-video-1",
            1_700_000_000_000L,
            RemoteIMOrigin.HUMAN
        );

        assertEquals(message.id(), again.id());
        assertEquals(1, state.messagesWith("mac-office").size());
    }
}
