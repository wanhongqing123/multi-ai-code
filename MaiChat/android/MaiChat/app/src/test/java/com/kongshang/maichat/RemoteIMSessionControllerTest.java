package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;

public class RemoteIMSessionControllerTest {
    @Test
    public void searchesHistoryAndOpensTheMatchedConversation() throws Exception {
        Path root = Files.createTempDirectory("maichat-android-session-search");
        RemoteIMSessionController session = newSession(root);
        session.login("android-user");
        session.addContact("alice");
        session.addContact("bob");
        session.chatState().selectPeer("alice");
        session.sendTextMessage("唯一搜索词");
        session.chatState().selectPeer("bob");

        List<RemoteIMMessageSearchHit> hits = session.searchMessages("唯一搜索词", 20);

        assertEquals(1, hits.size());
        assertEquals("alice", hits.get(0).peerUserId());
        assertEquals("alice", session.openMessageSearchHit(hits.get(0)).userId());
        assertEquals("alice", session.chatState().selectedPeerId());
    }

    @Test
    public void sendsTextWithQuoteSnapshotAttached() throws Exception {
        RemoteIMSessionController session = newSession();
        session.login("android-user");
        RemoteIMQuote quote = new RemoteIMQuote(
            "sdk-original-1",
            "mac-office",
            "原始消息",
            "text"
        );

        RemoteIMMessage sent = session.sendTextMessage("回复正文", quote);

        assertEquals(quote, sent.quote());
        assertEquals(RemoteIMMessage.Status.SENT, sent.status());
    }

    @Test
    public void broadcastQueuesSeparatePrivateMessagesAndDeduplicatesRecipients() throws Exception {
        Path root = Files.createTempDirectory("maichat-android-session-broadcast");
        RemoteIMSessionController session = newSession(root);
        session.login("android-user");
        session.addContact("alice");
        session.addContact("bob");
        session.chatState().selectPeer("mac-office");

        int count = session.broadcastText(
            List.of("alice", " bob ", "alice", ""),
            "群发正文",
            null
        );

        assertEquals(2, count);
        assertEquals(1, session.chatState().messagesWith("alice").size());
        assertEquals(1, session.chatState().messagesWith("bob").size());
        assertTrue(session.chatState().messagesWith("mac-office").isEmpty());
    }

    @Test
    public void startsLoggedOutWhenSettingsAreEmpty() throws Exception {
        RemoteIMSessionController session = newSession();

        assertTrue(session.requiresLogin());
        assertEquals("", session.settings().loginUserId());
    }

    @Test
    public void loginPersistsUserAndCreatesDefaultConversation() throws Exception {
        Path root = Files.createTempDirectory("maichat-android-session-login");
        RemoteIMSessionController session = newSession(root);

        session.login(" android-user ");

        assertFalse(session.requiresLogin());
        assertEquals("android-user", session.settings().loginUserId());
        assertEquals("android-user", session.chatState().ownerUserId());
        assertEquals("mac-office", session.chatState().selectedPeerId());
        assertEquals(1, session.chatState().contacts().size());

        RemoteIMSessionController restored = newSession(root);
        assertFalse(restored.requiresLogin());
        assertEquals("android-user", restored.settings().loginUserId());
    }

    @Test
    public void logoutClearsSettingsButKeepsSavedHistory() throws Exception {
        Path root = Files.createTempDirectory("maichat-android-session-logout");
        RemoteIMSessionController session = newSession(root);
        session.login("android-user");
        RemoteIMMessage sent = session.chatState().queueOutgoingText("ping");
        session.chatState().updateMessageStatus(sent.id(), RemoteIMMessage.Status.SENT);

        session.logout();

        assertTrue(session.requiresLogin());

        session.login("android-user");
        assertEquals(1, session.chatState().messagesWith("mac-office").size());
        assertEquals("ping", session.chatState().messagesWith("mac-office").get(0).text());
    }

    @Test
    public void sendTextMessageMarksMessageSentAndPersistsIt() throws Exception {
        Path root = Files.createTempDirectory("maichat-android-session-text");
        RemoteIMSessionController session = newSession(root);
        session.login("android-user");

        RemoteIMMessage message = session.sendTextMessage("检查构建");

        assertEquals(RemoteIMMessage.Status.SENT, message.status());
        assertEquals("检查构建", message.text());

        RemoteIMSessionController restored = newSession(root);
        assertEquals(1, restored.chatState().messagesWith("mac-office").size());
        assertEquals("检查构建", restored.chatState().messagesWith("mac-office").get(0).text());
    }

    @Test
    public void sendMediaMessagesMarkMessagesSent() throws Exception {
        Path root = Files.createTempDirectory("maichat-android-session-media");
        RemoteIMSessionController session = newSession(root);
        session.login("android-user");

        RemoteIMMessage image = session.sendImageMessage("/tmp/photo.png", 640, 480, 4096);
        RemoteIMMessage voice = session.sendVoiceMessage("/tmp/voice.m4a", 5);

        assertEquals(RemoteIMMessage.Status.SENT, image.status());
        assertEquals("[图片消息] photo.png", image.text());
        assertEquals(RemoteIMMessage.Status.SENT, voice.status());
        assertEquals("[语音消息 5s]", voice.text());
    }

    @Test
    public void sendApprovalDecisionMarksItSentAndPersistsCorrelation() throws Exception {
        Path root = Files.createTempDirectory("maichat-android-session-approval");
        RemoteIMSessionController session = newSession(root);
        session.login("android-user");
        RemoteIMApprovalRequest request = new RemoteIMApprovalRequest(
            "approval-session-1",
            List.of(RemoteIMApprovalAction.APPROVE_ONCE, RemoteIMApprovalAction.REJECT)
        );

        RemoteIMMessage message = session.sendApprovalDecision(
            "mac-office",
            request,
            RemoteIMApprovalAction.APPROVE_ONCE
        );

        assertEquals(RemoteIMMessage.Status.SENT, message.status());
        assertEquals("审批操作：同意本次", message.text());
        assertEquals(
            new RemoteIMApprovalDecision(
                request.token(),
                RemoteIMApprovalAction.APPROVE_ONCE
            ),
            message.approvalDecision()
        );

        RemoteIMSessionController restored = newSession(root);
        RemoteIMMessage restoredDecision = restored.chatState()
            .messagesWith("mac-office")
            .get(0);
        assertEquals(message.approvalDecision(), restoredDecision.approvalDecision());
        assertEquals(RemoteIMMessage.Status.SENT, restoredDecision.status());
    }

    private RemoteIMSessionController newSession() throws Exception {
        return newSession(Files.createTempDirectory("maichat-android-session-empty"));
    }

    private RemoteIMSessionController newSession(Path root) throws Exception {
        return new RemoteIMSessionController(
            new LocalSettingsStore(root.resolve("settings.properties").toFile()),
            new LocalChatHistoryStore(root.resolve("history").toFile())
        );
    }
}
