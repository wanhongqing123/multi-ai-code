package com.multiaicode.remoteim;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

import java.util.List;

public class ChatStateTest {
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
    public void messageStatusCanBeUpdated() {
        ChatState state = new ChatState("android-user");
        state.upsertContact(new RemoteIMContact("mac-office", "Mac Office"));
        state.selectPeer("mac-office");
        RemoteIMMessage message = state.queueOutgoingText("ping");

        assertTrue(state.updateMessageStatus(message.id(), RemoteIMMessage.Status.SENT));

        assertEquals(RemoteIMMessage.Status.SENT, state.messagesWith("mac-office").get(0).status());
        assertFalse(state.updateMessageStatus("missing", RemoteIMMessage.Status.SENT));
    }
}
