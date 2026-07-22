package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

import java.nio.file.Files;
import java.nio.file.Path;

public class RemoteIMSessionControllerTest {
    @Test
    public void startsLoggedOutWhenSettingsAreEmpty() throws Exception {
        RemoteIMSessionController session = newSession();

        assertTrue(session.requiresLogin());
        assertEquals("", session.settings().loginUserId());
    }

    @Test
    public void loginPersistsUserAndCreatesDefaultConversation() throws Exception {
        Path root = Files.createTempDirectory("multi-ai-code-android-session-login");
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
        Path root = Files.createTempDirectory("multi-ai-code-android-session-logout");
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
        Path root = Files.createTempDirectory("multi-ai-code-android-session-text");
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
        Path root = Files.createTempDirectory("multi-ai-code-android-session-media");
        RemoteIMSessionController session = newSession(root);
        session.login("android-user");

        RemoteIMMessage image = session.sendImageMessage("/tmp/photo.png", 640, 480, 4096);
        RemoteIMMessage voice = session.sendVoiceMessage("/tmp/voice.m4a", 5);

        assertEquals(RemoteIMMessage.Status.SENT, image.status());
        assertEquals("[图片消息] photo.png", image.text());
        assertEquals(RemoteIMMessage.Status.SENT, voice.status());
        assertEquals("[语音消息 5s]", voice.text());
    }

    private RemoteIMSessionController newSession() throws Exception {
        return newSession(Files.createTempDirectory("multi-ai-code-android-session-empty"));
    }

    private RemoteIMSessionController newSession(Path root) throws Exception {
        return new RemoteIMSessionController(
            new LocalSettingsStore(root.resolve("settings.properties").toFile()),
            new LocalChatHistoryStore(root.resolve("history").toFile())
        );
    }
}
