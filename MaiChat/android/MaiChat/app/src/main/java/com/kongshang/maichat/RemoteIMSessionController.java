package com.kongshang.maichat;

import android.content.Context;
import android.os.Handler;
import android.os.Looper;

import java.io.IOException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

public final class RemoteIMSessionController {
    public static final int SDK_APP_ID = 1_600_148_979;
    public static final String USER_SIG_SECRET_KEY =
        "aa18d554f5e4a235640745e98145e187977f87770b812b2b4f10ef032bd73861";
    static final String FALLBACK_OWNER_USER_ID = "android-user";
    static final String DEFAULT_CONTACT_USER_ID = "mac-office";

    public interface Listener {
        void onStateChanged();
        void onError(String message);
    }

    public interface BroadcastCompletion {
        void onFinished(int total, List<String> failedUserIds);
    }

    private final LocalSettingsStore settingsStore;
    private final LocalChatHistoryStore legacyHistoryStore;
    private final AndroidChatHistoryStore historyStore;
    private final TencentIMClient client;
    private final RemoteDesktopController remoteDesktop;
    private final Listener listener;
    private final Handler mainHandler;
    private final Map<String, Integer> unreadByUserId = new HashMap<>();
    private final Map<String, TencentIMClient.PresenceStatus> presenceByUserId = new HashMap<>();
    private final Map<String, Boolean> hasEarlierByUserId = new HashMap<>();
    private final boolean productionMode;

    private RemoteIMSettings settings;
    private ChatState chatState;
    private TencentIMClient.ConnectionState connectionState = TencentIMClient.ConnectionState.DISCONNECTED;
    private String connectionDetail = "未连接";
    private String visibleConversationUserId = "";
    private Runnable pendingStateNotification;

    public RemoteIMSessionController(
        LocalSettingsStore settingsStore,
        LocalChatHistoryStore historyStore
    ) {
        this.settingsStore = settingsStore;
        this.legacyHistoryStore = historyStore;
        this.historyStore = null;
        this.client = null;
        this.remoteDesktop = null;
        this.listener = null;
        this.mainHandler = null;
        this.productionMode = false;
        settings = loadSettings();
        chatState = loadLegacyChatState();
        ensureDefaultContactIfNeeded();
    }

    public RemoteIMSessionController(Context context, Listener listener) {
        Context applicationContext = context.getApplicationContext();
        this.settingsStore = new LocalSettingsStore(
            new java.io.File(applicationContext.getFilesDir(), "remote-im-settings/settings.properties")
        );
        this.legacyHistoryStore = null;
        this.historyStore = new AndroidChatHistoryStore(applicationContext);
        this.listener = listener;
        this.mainHandler = new Handler(Looper.getMainLooper());
        this.productionMode = true;
        this.client = new TencentIMClient(applicationContext, new ClientListener());
        this.remoteDesktop = new RemoteDesktopController(
            applicationContext,
            client,
            this::notifyStateChanged
        );
        settings = loadSettings();
        chatState = loadProductionChatState();
        if (!requiresLogin()) connect();
    }

    public RemoteIMSettings settings() {
        return settings;
    }

    public ChatState chatState() {
        return chatState;
    }

    public RemoteDesktopController remoteDesktop() {
        return remoteDesktop;
    }

    public TencentIMClient.ConnectionState connectionState() {
        return connectionState;
    }

    public String connectionDetail() {
        return connectionDetail;
    }

    public boolean requiresLogin() {
        return settings.requiresLogin();
    }

    public String currentUserSig() {
        if (requiresLogin()) return "";
        return TencentUserSigGenerator.generate(
            SDK_APP_ID,
            settings.loginUserId(),
            USER_SIG_SECRET_KEY
        );
    }

    public void login(String loginUserId) throws IOException {
        saveChatState();
        settings = new RemoteIMSettings(loginUserId);
        settingsStore.save(settings);
        chatState = productionMode ? loadProductionChatState() : loadLegacyChatState();
        unreadByUserId.clear();
        presenceByUserId.clear();
        hasEarlierByUserId.clear();
        if (productionMode) {
            connect();
        } else {
            ensureDefaultContactIfNeeded();
        }
        notifyStateChanged();
    }

    public void logout() throws IOException {
        saveChatState();
        settings = RemoteIMSettings.empty();
        settingsStore.save(settings);
        unreadByUserId.clear();
        presenceByUserId.clear();
        hasEarlierByUserId.clear();
        visibleConversationUserId = "";
        if (productionMode) {
            if (remoteDesktop != null) remoteDesktop.stop();
            client.disconnect(new EmptyOperationCompletion());
            connectionState = TencentIMClient.ConnectionState.DISCONNECTED;
            connectionDetail = "未连接";
            chatState = new ChatState(FALLBACK_OWNER_USER_ID);
        } else {
            chatState = loadLegacyChatState();
        }
        notifyStateChanged();
    }

    public void saveChatState() throws IOException {
        if (!productionMode && !requiresLogin()) legacyHistoryStore.save(chatState);
    }

    public void addContact(String userId) {
        String cleanUserId = clean(userId);
        if (cleanUserId.isEmpty() || requiresLogin()) return;
        RemoteIMContact contact = new RemoteIMContact(cleanUserId, cleanUserId);
        chatState.upsertContact(contact);
        chatState.selectPeer(cleanUserId);
        if (productionMode) {
            historyStore.upsertContact(chatState.ownerUserId(), contact);
            refreshContactMetadata(Collections.singletonList(cleanUserId));
        } else {
            try {
                saveChatState();
            } catch (IOException ignored) {
            }
        }
        notifyStateChanged();
    }

    public void deleteContact(String userId) {
        String cleanUserId = clean(userId);
        if (!productionMode) {
            chatState.removeContact(cleanUserId);
            try {
                saveChatState();
            } catch (IOException ignored) {
            }
            notifyStateChanged();
            return;
        }
        String ownerUserId = chatState.ownerUserId();
        client.deleteContact(cleanUserId, new TencentIMClient.OperationCompletion() {
            @Override
            public void onSuccess() {
                runOnMain(() -> {
                    if (!ownerUserId.equals(chatState.ownerUserId())) return;
                    chatState.removeContact(cleanUserId);
                    historyStore.deleteContact(chatState.ownerUserId(), cleanUserId);
                    unreadByUserId.remove(cleanUserId);
                    presenceByUserId.remove(cleanUserId);
                    hasEarlierByUserId.remove(cleanUserId);
                    notifyStateChanged();
                });
            }

            @Override
            public void onError(int code, String message) {
                if (!ownerUserId.equals(chatState.ownerUserId())) return;
                reportError(message == null || message.trim().isEmpty()
                    ? "删除好友失败（" + code + "）"
                    : message);
            }
        });
    }

    public boolean createContactGroup(String name) {
        String cleanName = ContactGroups.normalize(name);
        if (!ContactGroups.isAcceptableName(cleanName) || requiresLogin()) return false;
        boolean stored = productionMode
            ? historyStore.createContactGroup(chatState.ownerUserId(), cleanName)
            : chatState.addContactGroup(cleanName);
        if (!stored) return false;
        if (productionMode) chatState.addContactGroup(cleanName);
        persistLegacyAndNotify();
        return true;
    }

    public boolean renameContactGroup(String from, String to) {
        String oldName = ContactGroups.normalize(from);
        String newName = ContactGroups.normalize(to);
        if (!ContactGroups.isAcceptableName(newName) || requiresLogin()) return false;
        boolean stored = productionMode
            ? historyStore.renameContactGroup(chatState.ownerUserId(), oldName, newName)
            : chatState.renameContactGroup(oldName, newName);
        if (!stored) return false;
        if (productionMode) chatState.renameContactGroup(oldName, newName);
        persistLegacyAndNotify();
        return true;
    }

    public boolean deleteContactGroup(String name) {
        String cleanName = ContactGroups.normalize(name);
        if (cleanName.isEmpty() || requiresLogin()) return false;
        boolean stored = productionMode
            ? historyStore.deleteContactGroup(chatState.ownerUserId(), cleanName)
            : chatState.removeContactGroup(cleanName);
        if (!stored) return false;
        if (productionMode) chatState.removeContactGroup(cleanName);
        persistLegacyAndNotify();
        return true;
    }

    public boolean setContactGroup(String userId, String groupName) {
        String cleanUserId = clean(userId);
        if (cleanUserId.isEmpty() || requiresLogin()) return false;
        if (productionMode) {
            if (!historyStore.setContactGroup(
                chatState.ownerUserId(), cleanUserId, ContactGroups.normalize(groupName)
            )) return false;
        }
        if (!chatState.setContactGroup(cleanUserId, groupName)) return false;
        persistLegacyAndNotify();
        return true;
    }

    private void persistLegacyAndNotify() {
        if (!productionMode) {
            try {
                saveChatState();
            } catch (IOException ignored) {
            }
        }
        notifyStateChanged();
    }

    public void clearHistory(String userId) {
        String cleanUserId = clean(userId);
        if (!productionMode) {
            chatState.removeMessagesWith(cleanUserId);
            try {
                saveChatState();
            } catch (IOException ignored) {
            }
            notifyStateChanged();
            return;
        }
        String ownerUserId = chatState.ownerUserId();
        client.clearHistory(cleanUserId, new TencentIMClient.OperationCompletion() {
            @Override
            public void onSuccess() {
                runOnMain(() -> {
                    if (!ownerUserId.equals(chatState.ownerUserId())) return;
                    chatState.removeMessagesWith(cleanUserId);
                    historyStore.deleteConversation(chatState.ownerUserId(), cleanUserId);
                    unreadByUserId.remove(cleanUserId);
                    hasEarlierByUserId.put(cleanUserId, false);
                    notifyStateChanged();
                });
            }

            @Override
            public void onError(int code, String message) {
                if (!ownerUserId.equals(chatState.ownerUserId())) return;
                reportError(message == null || message.trim().isEmpty()
                    ? "清空聊天记录失败（" + code + "）"
                    : message);
            }
        });
    }

    public RemoteIMMessage sendTextMessage(String text) throws IOException {
        RemoteIMMessage message = chatState.queueOutgoingText(text);
        if (!productionMode) {
            markMessageSentAndSave(message);
            return message;
        }
        persistMessage(message);
        notifyStateChanged();
        client.sendText(
            message.toUserId(),
            message.text(),
            RemoteIMOrigin.HUMAN,
            sendCompletion(message)
        );
        return message;
    }

    public int broadcastText(
        List<String> rawUserIds,
        String text,
        BroadcastCompletion completion
    ) throws IOException {
        List<String> recipients = BroadcastSelectionPolicy.uniqueRecipientIds(rawUserIds);
        String cleanText = clean(text);
        if (recipients.isEmpty() || cleanText.isEmpty() || requiresLogin()) return 0;

        List<RemoteIMMessage> queued = new ArrayList<>();
        for (String userId : recipients) {
            RemoteIMMessage message = chatState.queueOutgoingTextTo(userId, cleanText);
            queued.add(message);
            if (productionMode) persistMessage(message);
        }
        notifyStateChanged();

        if (!productionMode) {
            for (RemoteIMMessage message : queued) {
                chatState.updateMessageStatus(message.id(), RemoteIMMessage.Status.SENT);
            }
            saveChatState();
            notifyStateChanged();
            if (completion != null) completion.onFinished(queued.size(), Collections.emptyList());
            return queued.size();
        }

        String ownerUserId = chatState.ownerUserId();
        BroadcastDeliveryTracker tracker = new BroadcastDeliveryTracker(
            queued.size(),
            (total, failed) -> {
                if (completion != null) completion.onFinished(total, failed);
            }
        );
        for (RemoteIMMessage message : queued) {
            client.sendText(
                message.toUserId(),
                message.text(),
                RemoteIMOrigin.HUMAN,
                new TencentIMClient.SendCompletion() {
                    @Override
                    public void onSuccess(String remoteId, long createdAtMillis) {
                        runOnMain(() -> {
                            if (!ownerUserId.equals(chatState.ownerUserId())) return;
                            chatState.updateMessageDelivery(message.id(), remoteId);
                            persistMessage(message);
                            notifyStateChanged();
                            tracker.record(message.toUserId(), true);
                        });
                    }

                    @Override
                    public void onError(int code, String description) {
                        runOnMain(() -> {
                            if (!ownerUserId.equals(chatState.ownerUserId())) return;
                            chatState.updateMessageStatus(message.id(), RemoteIMMessage.Status.FAILED);
                            persistMessage(message);
                            notifyStateChanged();
                            // 不逐条 reportError；等全部回执到齐后一次列出失败的人。
                            tracker.record(message.toUserId(), false);
                        });
                    }
                }
            );
        }
        return queued.size();
    }

    public RemoteIMMessage sendApprovalDecision(
        String peerId,
        RemoteIMApprovalRequest request,
        RemoteIMApprovalAction action
    ) throws IOException {
        if (request == null || !request.allows(action)) {
            throw new IllegalArgumentException("该审批请求不允许此操作");
        }
        RemoteIMMessage message = chatState.queueOutgoingApprovalDecision(
            peerId,
            request.token(),
            action
        );
        if (!productionMode) {
            markMessageSentAndSave(message);
            return message;
        }
        persistMessage(message);
        notifyStateChanged();
        try {
            client.sendApprovalDecision(
                peerId,
                message.approvalDecision(),
                sendCompletion(message)
            );
        } catch (RuntimeException error) {
            chatState.updateMessageStatus(message.id(), RemoteIMMessage.Status.FAILED);
            persistMessage(message);
            notifyStateChanged();
            throw error;
        }
        return message;
    }

    public void sendMachineText(String userId, String text) {
        if (!productionMode) return;
        client.sendText(userId, text, RemoteIMOrigin.MACHINE, new TencentIMClient.SendCompletion() {
            @Override
            public void onSuccess(String remoteId, long createdAtMillis) {
            }

            @Override
            public void onError(int code, String message) {
                reportError(message);
            }
        });
    }

    public RemoteIMMessage sendImageMessage(
        String localPath,
        int width,
        int height,
        long sizeBytes
    ) throws IOException {
        RemoteIMMessage message = chatState.queueOutgoingImage(localPath, width, height, sizeBytes);
        if (!productionMode) {
            markMessageSentAndSave(message);
            return message;
        }
        persistMessage(message);
        notifyStateChanged();
        client.sendImage(
            message.toUserId(),
            localPath,
            RemoteIMOrigin.HUMAN,
            sendCompletion(message)
        );
        return message;
    }

    public RemoteIMMessage sendVoiceMessage(String localPath, int durationSeconds) throws IOException {
        RemoteIMMessage message = chatState.queueOutgoingVoice(localPath, durationSeconds);
        if (!productionMode) {
            markMessageSentAndSave(message);
            return message;
        }
        persistMessage(message);
        notifyStateChanged();
        client.sendVoice(
            message.toUserId(),
            localPath,
            durationSeconds,
            RemoteIMOrigin.HUMAN,
            sendCompletion(message)
        );
        return message;
    }

    public RemoteIMMessage sendFileMessage(
        String localPath,
        String fileName,
        String mimeType,
        long sizeBytes
    ) throws IOException {
        RemoteIMMessage message = chatState.queueOutgoingFile(
            localPath,
            fileName,
            mimeType,
            sizeBytes
        );
        if (!productionMode) {
            markMessageSentAndSave(message);
            return message;
        }
        persistMessage(message);
        notifyStateChanged();
        client.sendFile(
            message.toUserId(),
            localPath,
            fileName,
            RemoteIMOrigin.HUMAN,
            sendCompletion(message)
        );
        return message;
    }

    public void selectContact(String userId) {
        chatState.selectPeer(userId);
        unreadByUserId.remove(clean(userId));
        notifyStateChanged();
    }

    public void setConversationVisible(String userId, boolean visible) {
        String cleanUserId = clean(userId);
        if (visible) {
            visibleConversationUserId = cleanUserId;
            unreadByUserId.remove(cleanUserId);
        } else if (visibleConversationUserId.equals(cleanUserId)) {
            visibleConversationUserId = "";
        }
    }

    public int unreadCount(String userId) {
        return unreadByUserId.getOrDefault(clean(userId), 0);
    }

    public int totalUnreadCount() {
        int total = 0;
        for (int count : unreadByUserId.values()) total += count;
        return total;
    }

    public TencentIMClient.PresenceStatus presenceStatus(String userId) {
        return presenceByUserId.getOrDefault(clean(userId), TencentIMClient.PresenceStatus.UNKNOWN);
    }

    public void loadInitialMessages(String userId) {
        if (!productionMode) return;
        String peerId = clean(userId);
        AndroidChatHistoryStore.Page page = historyStore.loadConversationPage(
            chatState.ownerUserId(),
            peerId,
            null,
            null,
            50
        );
        chatState.mergeMessages(page.messages());
        hasEarlierByUserId.put(peerId, page.hasEarlier());
    }

    public boolean loadEarlierMessages(String userId) {
        if (!productionMode) return false;
        String peerId = clean(userId);
        List<RemoteIMMessage> current = chatState.messagesWith(peerId);
        RemoteIMMessage oldest = current.isEmpty() ? null : current.get(0);
        AndroidChatHistoryStore.Page page = historyStore.loadConversationPage(
            chatState.ownerUserId(),
            peerId,
            oldest == null ? null : oldest.createdAtMillis(),
            oldest == null ? null : oldest.id(),
            50
        );
        chatState.mergeMessages(page.messages());
        hasEarlierByUserId.put(peerId, page.hasEarlier());
        notifyStateChanged();
        return !page.messages().isEmpty();
    }

    public boolean hasEarlierMessages(String userId) {
        return hasEarlierByUserId.getOrDefault(clean(userId), false);
    }

    public void destroy() {
        if (!productionMode) return;
        if (pendingStateNotification != null) {
            mainHandler.removeCallbacks(pendingStateNotification);
            pendingStateNotification = null;
        }
        if (remoteDesktop != null) remoteDesktop.destroy();
        client.destroy();
        historyStore.close();
    }

    private void connect() {
        try {
            client.connect(SDK_APP_ID, settings.loginUserId(), currentUserSig());
        } catch (RuntimeException error) {
            connectionState = TencentIMClient.ConnectionState.FAILED;
            connectionDetail = error.getMessage() == null ? "登录失败" : error.getMessage();
            reportError(connectionDetail);
        }
    }

    private void refreshContactMetadata() {
        if (!productionMode || chatState.contacts().isEmpty()) return;
        List<String> userIds = new ArrayList<>();
        for (RemoteIMContact contact : chatState.contacts()) userIds.add(contact.userId());
        refreshContactMetadata(userIds);
    }

    private void refreshContactMetadata(List<String> userIds) {
        if (!productionMode || userIds == null || userIds.isEmpty()) return;
        client.refreshProfiles(userIds);
        client.refreshAndSubscribePresence(userIds);
    }

    private TencentIMClient.SendCompletion sendCompletion(RemoteIMMessage message) {
        String ownerUserId = chatState.ownerUserId();
        return new TencentIMClient.SendCompletion() {
            @Override
            public void onSuccess(String remoteId, long createdAtMillis) {
                runOnMain(() -> {
                    if (!ownerUserId.equals(chatState.ownerUserId())) return;
                    chatState.updateMessageDelivery(message.id(), remoteId);
                    persistMessage(message);
                    notifyStateChanged();
                });
            }

            @Override
            public void onError(int code, String description) {
                runOnMain(() -> {
                    if (!ownerUserId.equals(chatState.ownerUserId())) return;
                    chatState.updateMessageStatus(message.id(), RemoteIMMessage.Status.FAILED);
                    persistMessage(message);
                    reportError(description == null || description.trim().isEmpty()
                        ? "消息发送失败（" + code + "）"
                        : description);
                    notifyStateChanged();
                });
            }
        };
    }

    private void receive(RemoteIMMessage incoming) {
        if (incoming == null || requiresLogin()) return;
        if (!chatState.ownerUserId().equals(incoming.toUserId())) return;
        if (incoming.text() != null
            && remoteDesktop != null
            && remoteDesktop.handleIncomingText(incoming.fromUserId(), incoming.text())) {
            return;
        }
        if (historyStore.containsRemoteId(chatState.ownerUserId(), incoming.remoteId())) return;
        boolean knownContact = contactExists(incoming.fromUserId());

        RemoteIMMessage message;
        if (incoming.imageAttachment() != null) {
            RemoteIMImageAttachment image = incoming.imageAttachment();
            message = chatState.receiveImage(
                image.localPath(),
                incoming.fromUserId(),
                image.width(),
                image.height(),
                image.sizeBytes(),
                incoming.remoteId(),
                incoming.createdAtMillis(),
                incoming.origin()
            );
        } else if (incoming.voiceAttachment() != null) {
            RemoteIMVoiceAttachment voice = incoming.voiceAttachment();
            message = chatState.receiveVoice(
                voice.localPath(),
                voice.durationSeconds(),
                incoming.fromUserId(),
                incoming.remoteId(),
                incoming.createdAtMillis(),
                incoming.origin()
            );
        } else if (incoming.videoAttachment() != null) {
            RemoteIMVideoAttachment video = incoming.videoAttachment();
            message = chatState.receiveVideo(
                video.localPath(),
                video.coverPath(),
                video.durationSeconds(),
                video.width(),
                video.height(),
                video.sizeBytes(),
                incoming.fromUserId(),
                incoming.remoteId(),
                incoming.createdAtMillis(),
                incoming.origin()
            );
        } else if (incoming.fileAttachment() != null) {
            RemoteIMFileAttachment file = incoming.fileAttachment();
            message = chatState.receiveFile(
                file.localPath(),
                incoming.fromUserId(),
                file.fileName(),
                file.mimeType(),
                file.sizeBytes(),
                incoming.remoteId(),
                incoming.createdAtMillis(),
                incoming.origin()
            );
        } else {
            message = chatState.receiveText(
                incoming.text(),
                incoming.fromUserId(),
                incoming.remoteId(),
                incoming.createdAtMillis(),
                incoming.origin(),
                incoming.approvalRequest(),
                incoming.approvalDecision()
            );
        }
        RemoteIMContact contact = findContact(incoming.fromUserId());
        historyStore.upsertContact(chatState.ownerUserId(), contact);
        persistMessage(message);
        if (!visibleConversationUserId.equals(incoming.fromUserId())) {
            unreadByUserId.put(
                incoming.fromUserId(),
                unreadByUserId.getOrDefault(incoming.fromUserId(), 0) + 1
            );
        }
        if (!knownContact) {
            refreshContactMetadata(Collections.singletonList(incoming.fromUserId()));
        }
        notifyStateChanged();
    }

    private boolean contactExists(String userId) {
        for (RemoteIMContact contact : chatState.contacts()) {
            if (contact.userId().equals(userId)) return true;
        }
        return false;
    }

    private RemoteIMContact findContact(String userId) {
        for (RemoteIMContact contact : chatState.contacts()) {
            if (contact.userId().equals(userId)) return contact;
        }
        return new RemoteIMContact(userId, userId);
    }

    private void persistMessage(RemoteIMMessage message) {
        if (productionMode) historyStore.upsertMessage(chatState.ownerUserId(), message);
    }

    private void markMessageSentAndSave(RemoteIMMessage message) throws IOException {
        chatState.updateMessageStatus(message.id(), RemoteIMMessage.Status.SENT);
        saveChatState();
    }

    private RemoteIMSettings loadSettings() {
        try {
            return settingsStore.load();
        } catch (IOException error) {
            return RemoteIMSettings.empty();
        }
    }

    private ChatState loadLegacyChatState() {
        String ownerUserId = requiresLogin() ? FALLBACK_OWNER_USER_ID : settings.loginUserId();
        try {
            return legacyHistoryStore.load(ownerUserId);
        } catch (IOException error) {
            return new ChatState(ownerUserId);
        }
    }

    private ChatState loadProductionChatState() {
        String ownerUserId = requiresLogin() ? FALLBACK_OWNER_USER_ID : settings.loginUserId();
        if (requiresLogin()) return new ChatState(ownerUserId);
        ChatState state = new ChatState(ownerUserId);
        state.setContactGroups(historyStore.loadContactGroups(ownerUserId));
        for (RemoteIMContact contact : historyStore.loadContacts(ownerUserId)) {
            state.upsertContact(contact);
        }
        state.mergeMessages(historyStore.loadConversationSummaries(ownerUserId));
        if (!state.contacts().isEmpty()) state.selectPeer(state.contacts().get(0).userId());
        return state;
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

    private void notifyStateChanged() {
        if (listener == null) return;
        if (mainHandler == null) {
            listener.onStateChanged();
            return;
        }
        Runnable schedule = () -> {
            if (pendingStateNotification != null) {
                mainHandler.removeCallbacks(pendingStateNotification);
            }
            pendingStateNotification = () -> {
                pendingStateNotification = null;
                listener.onStateChanged();
            };
            mainHandler.postDelayed(pendingStateNotification, 100);
        };
        if (Looper.myLooper() == Looper.getMainLooper()) schedule.run();
        else mainHandler.post(schedule);
    }

    private void reportError(String message) {
        if (listener == null) return;
        runOnMain(() -> listener.onError(message == null ? "操作失败" : message));
    }

    private void runOnMain(Runnable runnable) {
        if (mainHandler == null || Looper.myLooper() == Looper.getMainLooper()) {
            runnable.run();
        } else {
            mainHandler.post(runnable);
        }
    }

    private static String clean(String value) {
        return value == null ? "" : value.trim();
    }

    private final class ClientListener implements TencentIMClient.Listener {
        @Override
        public void onConnectionStateChanged(
            TencentIMClient.ConnectionState state,
            String detail
        ) {
            runOnMain(() -> {
                connectionState = state;
                connectionDetail = detail;
                if (state == TencentIMClient.ConnectionState.CONNECTED) refreshContactMetadata();
                notifyStateChanged();
            });
        }

        @Override
        public void onIncomingMessage(RemoteIMMessage message) {
            runOnMain(() -> receive(message));
        }

        @Override
        public void onProfilesUpdated(List<RemoteIMContact> contacts) {
            runOnMain(() -> {
                for (RemoteIMContact contact : contacts) {
                    chatState.upsertContact(contact);
                    historyStore.upsertContact(chatState.ownerUserId(), contact);
                }
                notifyStateChanged();
            });
        }

        @Override
        public void onPresenceUpdated(Map<String, TencentIMClient.PresenceStatus> statuses) {
            runOnMain(() -> {
                presenceByUserId.putAll(statuses);
                notifyStateChanged();
            });
        }
    }

    private static final class EmptyOperationCompletion implements TencentIMClient.OperationCompletion {
        @Override
        public void onSuccess() {
        }

        @Override
        public void onError(int code, String message) {
        }
    }
}
