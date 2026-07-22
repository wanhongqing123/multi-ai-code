package com.kongshang.maichat;

import java.io.IOException;

public final class RemoteIMSessionController {
    static final String FALLBACK_OWNER_USER_ID = "android-user";
    static final String DEFAULT_CONTACT_USER_ID = "mac-office";

    private final LocalSettingsStore settingsStore;
    private final LocalChatHistoryStore historyStore;
    private RemoteIMSettings settings;
    private ChatState chatState;

    public RemoteIMSessionController(
        LocalSettingsStore settingsStore,
        LocalChatHistoryStore historyStore
    ) {
        this.settingsStore = settingsStore;
        this.historyStore = historyStore;
        settings = loadSettings();
        chatState = loadChatState();
        ensureDefaultContactIfNeeded();
    }

    public RemoteIMSettings settings() {
        return settings;
    }

    public ChatState chatState() {
        return chatState;
    }

    public boolean requiresLogin() {
        return settings.requiresLogin();
    }

    public void login(String loginUserId) throws IOException {
        saveChatState();
        settings = new RemoteIMSettings(loginUserId);
        settingsStore.save(settings);
        chatState = loadChatState();
        ensureDefaultContactIfNeeded();
    }

    public void logout() throws IOException {
        saveChatState();
        settings = RemoteIMSettings.empty();
        settingsStore.save(settings);
        chatState = loadChatState();
    }

    public void saveChatState() throws IOException {
        if (!requiresLogin()) {
            historyStore.save(chatState);
        }
    }

    public RemoteIMMessage sendTextMessage(String text) throws IOException {
        RemoteIMMessage message = chatState.queueOutgoingText(text);
        markMessageSentAndSave(message);
        return message;
    }

    public RemoteIMMessage sendImageMessage(
        String localPath,
        int width,
        int height,
        long sizeBytes
    ) throws IOException {
        RemoteIMMessage message = chatState.queueOutgoingImage(localPath, width, height, sizeBytes);
        markMessageSentAndSave(message);
        return message;
    }

    public RemoteIMMessage sendVoiceMessage(String localPath, int durationSeconds) throws IOException {
        RemoteIMMessage message = chatState.queueOutgoingVoice(localPath, durationSeconds);
        markMessageSentAndSave(message);
        return message;
    }

    private void markMessageSentAndSave(RemoteIMMessage message) throws IOException {
        chatState.updateMessageStatus(message.id(), RemoteIMMessage.Status.SENT);
        saveChatState();
    }

    private RemoteIMSettings loadSettings() {
        try {
            return settingsStore.load();
        } catch (IOException err) {
            return RemoteIMSettings.empty();
        }
    }

    private ChatState loadChatState() {
        String ownerUserId = requiresLogin()
            ? FALLBACK_OWNER_USER_ID
            : settings.loginUserId();
        try {
            return historyStore.load(ownerUserId);
        } catch (IOException err) {
            return new ChatState(ownerUserId);
        }
    }

    private void ensureDefaultContactIfNeeded() {
        if (requiresLogin()) return;
        if (chatState.contacts().isEmpty()) {
            chatState.upsertContact(new RemoteIMContact(DEFAULT_CONTACT_USER_ID, DEFAULT_CONTACT_USER_ID));
            chatState.selectPeer(DEFAULT_CONTACT_USER_ID);
        } else if (chatState.selectedPeerId() == null) {
            chatState.selectPeer(chatState.contacts().get(0).userId());
        }
    }
}
