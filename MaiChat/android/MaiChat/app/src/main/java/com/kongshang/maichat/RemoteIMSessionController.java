package com.kongshang.maichat;

import android.content.Context;
import android.os.Handler;
import android.os.Looper;

import java.io.File;
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
        default void onActivityChanged(String peerId) { }

        default void onNewIncomingMessage(
            RemoteIMMessage message,
            boolean conversationVisible
        ) {
        }
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
    private final Map<String, Long> oldestLoadedCreatedAtByUserId = new HashMap<>();
    private final Map<String, String> oldestLoadedMessageIdByUserId = new HashMap<>();
    private final boolean productionMode;

    private volatile RemoteIMSettings settings;
    private volatile ChatState chatState;
    private volatile boolean destroyed;
    private int accountGeneration;
    private final Map<String, RemoteIMVideoAttachment> pendingVideoMedia = new java.util.LinkedHashMap<>();
    private int mediaRevision;
    public int mediaRevision() { return mediaRevision; }
    private boolean restoringAccount;
    private final java.util.concurrent.ExecutorService ioExecutor = java.util.concurrent.Executors.newSingleThreadExecutor();
    private final java.util.Set<String> loadingPeers = new java.util.HashSet<>();
    private final Map<String, Integer> pageRequests = new HashMap<>();
    private final Map<String, Integer> peerEpochs = new HashMap<>();
    private interface IOAction { void run() throws Exception; }

    private TencentIMClient.ConnectionState connectionState = TencentIMClient.ConnectionState.DISCONNECTED;
    private String connectionDetail = "未连接";
    private String visibleConversationUserId = "";
    private Runnable pendingStateNotification;
    private final RemoteIMActivityState activities = new RemoteIMActivityState();
    private Runnable activityExpiry;
    private Runnable typingIdle;
    private String typingPeer = "", typingId = "";
    private long typingSequence, typingLastSent;


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
        settings = RemoteIMSettings.empty();
        chatState = new ChatState(FALLBACK_OWNER_USER_ID);
        restoringAccount = true;
        int generation = ++accountGeneration;
        enqueueIO(() -> {
            RemoteIMSettings restoredSettings = loadSettings();
            ChatState restored = readProductionChatState(restoredSettings);
            runOnMain(() -> {
                if (generation != accountGeneration) return;
                settings = restoredSettings; chatState = restored; restoringAccount = false;
                notifyStateChanged();
                if (!requiresLogin()) connect();
            });
        });
    }

    int accountGeneration() { return accountGeneration; }
    boolean acceptsOutgoing(int generation, String owner, String peer) {
        return !destroyed && !requiresLogin() && generation == accountGeneration
            && chatState.ownerUserId().equals(owner) && contactExists(peer);
    }

    public boolean isRestoringAccount() { return restoringAccount; }
    public boolean isLoadingMessages(String peer) { return loadingPeers.contains(clean(peer)); }

    private void enqueueIO(IOAction action) {
        if (destroyed) return;
        int generation = accountGeneration;
        ioExecutor.execute(() -> {
            try { action.run(); }
            catch (Exception error) {
                runOnMain(() -> {
                    if (generation != accountGeneration) return;
                    restoringAccount = false;
                    reportError("本地数据操作失败：" + error.getMessage());
                    notifyStateChanged();
                });
            }
        });
    }

    private void switchAccount(RemoteIMSettings next) {
        resetActivities();
        int generation = ++accountGeneration;
        settings = next;
        chatState = new ChatState(next.requiresLogin() ? FALLBACK_OWNER_USER_ID : next.loginUserId());
        unreadByUserId.clear(); presenceByUserId.clear(); hasEarlierByUserId.clear();
        oldestLoadedCreatedAtByUserId.clear(); oldestLoadedMessageIdByUserId.clear();
        loadingPeers.clear(); pageRequests.clear(); peerEpochs.clear(); pendingVideoMedia.clear(); mediaRevision++;
        visibleConversationUserId = "";
        restoringAccount = !next.requiresLogin();
        if (next.requiresLogin()) {
            if (remoteDesktop != null) remoteDesktop.stop();
            client.disconnect(new EmptyOperationCompletion());
            connectionState = TencentIMClient.ConnectionState.DISCONNECTED;
            connectionDetail = "未连接";
        }
        enqueueIO(() -> {
            settingsStore.save(next);
            if (next.requiresLogin()) return;
            ChatState restored = readProductionChatState(next);
            runOnMain(() -> {
                if (generation != accountGeneration) return;
                chatState = restored; restoringAccount = false;
                connect(); notifyStateChanged();
            });
        });
        notifyStateChanged();
    }

    private void persistContact(RemoteIMContact contact) {
        String owner = chatState.ownerUserId();
        enqueueIO(() -> historyStore.upsertContact(owner, contact));
    }

    public RemoteIMActivitySignal activity(String peerId) { return activities.get(clean(peerId)); }

    public void updateHumanTyping(String peerId, boolean active) {
        if (!productionMode) return;
        String peer = clean(peerId);
        if (!active || !peer.equals(typingPeer)) stopHumanTyping();
        if (!active || peer.isEmpty() || connectionState != TencentIMClient.ConnectionState.CONNECTED) return;
        long now = android.os.SystemClock.elapsedRealtime();
        if (typingId.isEmpty()) {
            typingId = "typing:" + java.util.UUID.randomUUID();
            typingPeer = peer; typingSequence = 0; typingLastSent = now - 3000;
        }
        if (now - typingLastSent >= 3000) {
            typingLastSent = now;
            client.sendActivity(peer, new RemoteIMActivitySignal(typingId, ++typingSequence,
                RemoteIMActivitySignal.Kind.HUMAN_TYPING, true, 12000));
        }
        if (typingIdle != null) mainHandler.removeCallbacks(typingIdle);
        typingIdle = this::stopHumanTyping;
        mainHandler.postDelayed(typingIdle, 4000);
    }

    public void stopHumanTyping() {
        if (!productionMode) return;
        if (typingIdle != null) mainHandler.removeCallbacks(typingIdle);
        typingIdle = null;
        if (!typingId.isEmpty() && connectionState == TencentIMClient.ConnectionState.CONNECTED) {
            client.sendActivity(typingPeer, new RemoteIMActivitySignal(typingId, ++typingSequence,
                RemoteIMActivitySignal.Kind.HUMAN_TYPING, false, 1000));
        }
        typingId = ""; typingPeer = "";
    }

    private void resetActivities() {
        stopHumanTyping();
        if (activityExpiry != null) mainHandler.removeCallbacks(activityExpiry);
        activityExpiry = null;
        activities.reset();
    }

    private void scheduleActivityExpiry() {
        if (activityExpiry != null) mainHandler.removeCallbacks(activityExpiry);
        long next = activities.nextExpiry();
        if (next == Long.MAX_VALUE) { activityExpiry = null; return; }
        activityExpiry = () -> {
            for (String peer : activities.expire(android.os.SystemClock.elapsedRealtime())) {
                if (listener != null) listener.onActivityChanged(peer);
            }
            scheduleActivityExpiry();
        };
        mainHandler.postDelayed(activityExpiry, Math.max(1, next - android.os.SystemClock.elapsedRealtime()));
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
        if (productionMode) { switchAccount(new RemoteIMSettings(loginUserId)); return; }
        saveChatState();
        settings = new RemoteIMSettings(loginUserId);
        settingsStore.save(settings);
        chatState = loadLegacyChatState();
        unreadByUserId.clear();
        presenceByUserId.clear();
        hasEarlierByUserId.clear();
        oldestLoadedCreatedAtByUserId.clear();
        oldestLoadedMessageIdByUserId.clear();
        if (productionMode) {
            connect();
        } else {
            ensureDefaultContactIfNeeded();
        }
        notifyStateChanged();
    }

    public void logout() throws IOException {
        if (productionMode) { switchAccount(RemoteIMSettings.empty()); return; }
        saveChatState();
        settings = RemoteIMSettings.empty();
        settingsStore.save(settings);
        unreadByUserId.clear();
        presenceByUserId.clear();
        hasEarlierByUserId.clear();
        oldestLoadedCreatedAtByUserId.clear();
        oldestLoadedMessageIdByUserId.clear();
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
            persistContact(contact);
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
                    peerEpochs.put(cleanUserId, peerEpochs.getOrDefault(cleanUserId, 0) + 1);
                    pageRequests.put(cleanUserId, pageRequests.getOrDefault(cleanUserId, 0) + 1);
                    loadingPeers.remove(cleanUserId);
                    enqueueIO(() -> historyStore.deleteContact(ownerUserId, cleanUserId));
                    unreadByUserId.remove(cleanUserId);
                    presenceByUserId.remove(cleanUserId);
                    hasEarlierByUserId.remove(cleanUserId);
                    oldestLoadedCreatedAtByUserId.remove(cleanUserId);
                    oldestLoadedMessageIdByUserId.remove(cleanUserId);
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
        if (!chatState.addContactGroup(cleanName)) return false;
        if (productionMode) {
            String owner = chatState.ownerUserId();
            enqueueIO(() -> historyStore.createContactGroup(owner, cleanName));
        }
        persistLegacyAndNotify();
        return true;
    }

    public boolean renameContactGroup(String from, String to) {
        String oldName = ContactGroups.normalize(from);
        String newName = ContactGroups.normalize(to);
        if (!ContactGroups.isAcceptableName(newName) || requiresLogin()) return false;
        if (!chatState.renameContactGroup(oldName, newName)) return false;
        if (productionMode) {
            String owner = chatState.ownerUserId();
            enqueueIO(() -> historyStore.renameContactGroup(owner, oldName, newName));
        }
        persistLegacyAndNotify();
        return true;
    }

    public boolean deleteContactGroup(String name) {
        String cleanName = ContactGroups.normalize(name);
        if (cleanName.isEmpty() || requiresLogin()) return false;
        if (!chatState.removeContactGroup(cleanName)) return false;
        if (productionMode) {
            String owner = chatState.ownerUserId();
            enqueueIO(() -> historyStore.deleteContactGroup(owner, cleanName));
        }
        persistLegacyAndNotify();
        return true;
    }

    public boolean setContactGroup(String userId, String groupName) {
        String cleanUserId = clean(userId);
        if (cleanUserId.isEmpty() || requiresLogin()) return false;
        if (!chatState.setContactGroup(cleanUserId, groupName)) return false;
        if (productionMode) {
            String owner = chatState.ownerUserId();
            enqueueIO(() -> historyStore.setContactGroup(owner, cleanUserId, ContactGroups.normalize(groupName)));
        }
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
                    peerEpochs.put(cleanUserId, peerEpochs.getOrDefault(cleanUserId, 0) + 1);
                    pageRequests.put(cleanUserId, pageRequests.getOrDefault(cleanUserId, 0) + 1);
                    loadingPeers.remove(cleanUserId);
                    enqueueIO(() -> historyStore.deleteConversation(ownerUserId, cleanUserId));
                    unreadByUserId.remove(cleanUserId);
                    hasEarlierByUserId.put(cleanUserId, false);
                    oldestLoadedCreatedAtByUserId.remove(cleanUserId);
                    oldestLoadedMessageIdByUserId.remove(cleanUserId);
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
        return sendTextMessage(text, null);
    }

    public RemoteIMMessage sendTextMessage(String text, RemoteIMQuote quote) throws IOException {
        return sendTextMessageTo(chatState.selectedPeerId(), text, quote);
    }

    public RemoteIMMessage sendTextMessageTo(String peer, String text, RemoteIMQuote quote) throws IOException {
        stopHumanTyping();
        RemoteIMMessage message = chatState.queueOutgoingTextTo(peer, text);
        message.setQuote(quote);
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
            quote,
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
        return sendImageMessageTo(chatState.selectedPeerId(), localPath, width, height, sizeBytes);
    }

    public RemoteIMMessage sendImageMessageTo(String peer, String localPath, int width, int height, long sizeBytes) throws IOException {
        return sendImageMessageTo(peer, localPath, width, height, sizeBytes, null);
    }

    public RemoteIMMessage sendImageMessageTo(String peer, String localPath, int width, int height, long sizeBytes, RemoteIMQuote quote) throws IOException {
        RemoteIMMessage message = chatState.queueOutgoingImageTo(peer, localPath, width, height, sizeBytes);
        message.setQuote(quote);
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
            quote,
            sendCompletion(message)
        );
        return message;
    }

    public RemoteIMMessage sendVoiceMessage(String localPath, int durationSeconds) throws IOException {
        return sendVoiceMessageTo(chatState.selectedPeerId(), localPath, durationSeconds);
    }

    public RemoteIMMessage sendVoiceMessageTo(String peer, String localPath, int durationSeconds) throws IOException {
        return sendVoiceMessageTo(peer, localPath, durationSeconds, null);
    }

    public RemoteIMMessage sendVoiceMessageTo(String peer, String localPath, int durationSeconds, RemoteIMQuote quote) throws IOException {
        RemoteIMMessage message = chatState.queueOutgoingVoiceTo(peer, localPath, durationSeconds);
        message.setQuote(quote);
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
            quote,
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
        return sendFileMessageTo(chatState.selectedPeerId(), localPath, fileName, mimeType, sizeBytes);
    }

    public RemoteIMMessage sendFileMessageTo(String peer, String localPath, String fileName, String mimeType, long sizeBytes) throws IOException {
        return sendFileMessageTo(peer, localPath, fileName, mimeType, sizeBytes, null);
    }

    public RemoteIMMessage sendFileMessageTo(String peer, String localPath, String fileName, String mimeType, long sizeBytes, RemoteIMQuote quote) throws IOException {
        RemoteIMMessage message = chatState.queueOutgoingFileTo(
            peer, localPath,
            fileName,
            mimeType,
            sizeBytes
        );
        message.setQuote(quote);
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
            quote,
            sendCompletion(message)
        );
        return message;
    }

    public RemoteIMMessage sendVideoMessageTo(String peer, RemoteIMVideoAttachment attachment) throws IOException {
        return sendVideoMessageTo(peer, attachment, null);
    }

    public RemoteIMMessage sendVideoMessageTo(String peer, RemoteIMVideoAttachment attachment, RemoteIMQuote quote) throws IOException {
        RemoteIMMessage message = chatState.queueOutgoingVideoTo(peer, attachment);
        message.setQuote(quote);
        if (!productionMode) { markMessageSentAndSave(message); return message; }
        persistMessage(message); notifyStateChanged();
        client.sendVideo(peer, attachment, RemoteIMOrigin.HUMAN, quote, sendCompletion(message));
        return message;
    }

    public void forwardMessageAsync(RemoteIMMessage source, String targetUserId,
        java.util.function.Consumer<RemoteIMMessage> completed, java.util.function.Consumer<String> failed) {
        if (source == null) { failed.accept("消息不存在"); return; }
        RemoteIMMessage snapshot = source.snapshot();
        int generation = accountGeneration;
        enqueueIO(() -> {
            try {
                if (snapshot.imageAttachment() != null) requireForwardingFile(snapshot.imageAttachment().localPath(), "图片");
                if (snapshot.voiceAttachment() != null) requireForwardingFile(snapshot.voiceAttachment().localPath(), "语音");
                if (snapshot.videoAttachment() != null) requireForwardingFile(snapshot.videoAttachment().localPath(), "视频");
                if (snapshot.fileAttachment() != null) requireForwardingFile(snapshot.fileAttachment().localPath(), "文件");
                runOnMain(() -> {
                    if (generation != accountGeneration) return;
                    try { completed.accept(forwardMessage(snapshot, targetUserId, true)); }
                    catch (IOException | RuntimeException error) { failed.accept(error.getMessage()); }
                });
            } catch (Exception error) { runOnMain(() -> { if (generation == accountGeneration) failed.accept(error.getMessage()); }); }
        });
    }

    public RemoteIMMessage forwardMessage(RemoteIMMessage source, String targetUserId)
        throws IOException { return forwardMessage(source, targetUserId, false); }

    private RemoteIMMessage forwardMessage(RemoteIMMessage source, String targetUserId, boolean validated)
        throws IOException {
        if (source == null) throw new IllegalArgumentException("message is required");
        String target = clean(targetUserId);
        if (target.isEmpty() || target.equals(chatState.ownerUserId())) {
            throw new IllegalArgumentException("target user is required");
        }

        RemoteIMMessage forwarded;
        if (source.imageAttachment() != null) {
            RemoteIMImageAttachment attachment = source.imageAttachment();
            if (!validated) requireForwardingFile(attachment.localPath(), "图片");
            forwarded = chatState.queueOutgoingImageTo(
                target,
                attachment.localPath(),
                attachment.width(),
                attachment.height(),
                attachment.sizeBytes()
            );
        } else if (source.voiceAttachment() != null) {
            RemoteIMVoiceAttachment attachment = source.voiceAttachment();
            if (!validated) requireForwardingFile(attachment.localPath(), "语音");
            forwarded = chatState.queueOutgoingVoiceTo(
                target,
                attachment.localPath(),
                attachment.durationSeconds()
            );
        } else if (source.fileAttachment() != null) {
            RemoteIMFileAttachment attachment = source.fileAttachment();
            if (!validated) requireForwardingFile(attachment.localPath(), "文件");
            forwarded = chatState.queueOutgoingFileTo(
                target,
                attachment.localPath(),
                attachment.fileName(),
                attachment.mimeType(),
                attachment.sizeBytes()
            );
        } else if (source.videoAttachment() != null) {
            RemoteIMVideoAttachment attachment = source.videoAttachment();
            if (!validated) requireForwardingFile(attachment.localPath(), "视频");
            forwarded = chatState.queueOutgoingVideoTo(target, attachment);
        } else {
            forwarded = chatState.queueOutgoingTextTo(target, source.text());
        }

        if (!productionMode) {
            markMessageSentAndSave(forwarded);
            return forwarded;
        }
        persistMessage(forwarded);
        notifyStateChanged();
        TencentIMClient.SendCompletion completion = sendCompletion(forwarded);
        if (forwarded.imageAttachment() != null) {
            client.sendImage(target, forwarded.imageAttachment().localPath(),
                RemoteIMOrigin.HUMAN, completion);
        } else if (forwarded.voiceAttachment() != null) {
            client.sendVoice(target, forwarded.voiceAttachment().localPath(),
                forwarded.voiceAttachment().durationSeconds(), RemoteIMOrigin.HUMAN, completion);
        } else if (forwarded.videoAttachment() != null) {
            client.sendVideo(target, forwarded.videoAttachment(), RemoteIMOrigin.HUMAN, completion);
        } else if (forwarded.fileAttachment() != null) {
            client.sendFile(target, forwarded.fileAttachment().localPath(),
                forwarded.fileAttachment().fileName(), RemoteIMOrigin.HUMAN, completion);
        } else {
            client.sendText(target, forwarded.text(), RemoteIMOrigin.HUMAN, completion);
        }
        return forwarded;
    }

    private static void requireForwardingFile(String path, String type) throws IOException {
        if (path == null || path.trim().isEmpty() || !new File(path).isFile()) {
            throw new IOException(type + "尚未下载完成或本地缓存已被清理");
        }
    }

    public void selectContact(String userId) {
        chatState.selectPeer(userId);
        unreadByUserId.remove(clean(userId));
        notifyStateChanged();
    }

    public void setConversationVisible(String userId, boolean visible) {
        if (!visible || !clean(userId).equals(visibleConversationUserId)) stopHumanTyping();
        String cleanUserId = clean(userId);
        if (visible) {
            visibleConversationUserId = cleanUserId;
            unreadByUserId.remove(cleanUserId);
        } else if (visibleConversationUserId.equals(cleanUserId)) {
            visibleConversationUserId = "";
            if (productionMode) chatState.retainRecentMessages(cleanUserId, 20);
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

    public void loadInitialMessages(String userId) { loadMessagePage(userId, true); }

    public boolean loadEarlierMessages(String userId) {
        if (!productionMode || isLoadingMessages(userId)) return false;
        loadMessagePage(userId, false);
        return true;
    }

    private void loadMessagePage(String userId, boolean initial) {
        if (!productionMode || requiresLogin()) return;
        String peer = clean(userId), owner = chatState.ownerUserId();
        if (peer.isEmpty()) return;
        int generation = accountGeneration;
        int request = pageRequests.getOrDefault(peer, 0) + 1;
        pageRequests.put(peer, request); loadingPeers.add(peer);
        Long cursorTime = initial ? null : oldestLoadedCreatedAtByUserId.get(peer);
        String cursorId = initial ? null : oldestLoadedMessageIdByUserId.get(peer);
        enqueueIO(() -> {
            AndroidChatHistoryStore.Page page;
            try { page = historyStore.loadConversationPage(owner, peer, cursorTime, cursorId, 20); }
            catch (RuntimeException error) {
                runOnMain(() -> { if (generation == accountGeneration && pageRequests.getOrDefault(peer, 0) == request) loadingPeers.remove(peer); });
                throw error;
            }
            runOnMain(() -> {
                if (generation != accountGeneration || pageRequests.getOrDefault(peer, 0) != request) return;
                loadingPeers.remove(peer);
                java.util.Set<String> loadedIds = new java.util.HashSet<>();
                for (RemoteIMMessage message : chatState.messages()) loadedIds.add(message.id());
                List<RemoteIMMessage> additional = new ArrayList<>();
                for (RemoteIMMessage message : page.messages()) {
                    if (!loadedIds.contains(message.id()) && (message.remoteId().isEmpty()
                        || chatState.messageWithRemoteId(message.remoteId()) == null)) additional.add(message);
                }
                chatState.mergeMessages(additional);
                hasEarlierByUserId.put(peer, page.hasEarlier());
                updateOldestLoadedCursor(peer, page.messages());
                notifyStateChanged();
            });
        });
    }

    public List<RemoteIMMessageSearchHit> searchMessages(String query, int limit) {
        String cleanQuery = clean(query);
        if (cleanQuery.isEmpty() || requiresLogin()) return Collections.emptyList();
        if (productionMode) {
            return historyStore.searchMessages(chatState.ownerUserId(), cleanQuery, limit);
        }
        List<RemoteIMMessageSearchHit> hits = new ArrayList<>();
        String lowerQuery = cleanQuery.toLowerCase(java.util.Locale.ROOT);
        for (RemoteIMContact contact : chatState.contacts()) {
            for (RemoteIMMessage message : chatState.messagesWith(contact.userId())) {
                if (message.text().toLowerCase(java.util.Locale.ROOT).contains(lowerQuery)) {
                    hits.add(new RemoteIMMessageSearchHit(contact.userId(), message));
                }
            }
        }
        hits.sort((left, right) -> Long.compare(
            right.message().createdAtMillis(),
            left.message().createdAtMillis()
        ));
        return Collections.unmodifiableList(
            new ArrayList<>(hits.subList(0, Math.min(Math.max(limit, 1), hits.size())))
        );
    }

    public RemoteIMContact openMessageSearchHit(RemoteIMMessageSearchHit hit) {
        if (hit == null) return null;
        RemoteIMContact contact = null;
        for (RemoteIMContact candidate : chatState.contacts()) {
            if (candidate.userId().equals(hit.peerUserId())) {
                contact = candidate;
                break;
            }
        }
        if (contact == null) return null;
        selectContact(contact.userId());
        loadInitialMessages(contact.userId());
        chatState.mergeMessages(Collections.singletonList(hit.message()));
        return contact;
    }

    public RemoteIMMessage findQuotedMessage(String peerUserId, String remoteId) {
        String cleanPeerId = clean(peerUserId);
        String cleanRemoteId = clean(remoteId);
        if (cleanPeerId.isEmpty() || cleanRemoteId.isEmpty()) return null;
        RemoteIMMessage inMemory = chatState.messageWithRemoteId(cleanRemoteId);
        if (inMemory != null) return inMemory;
        if (!productionMode) return null;
        return null;
    }

    public void findQuotedMessage(String peer, String remoteId, java.util.function.Consumer<RemoteIMMessage> completion) {
        RemoteIMMessage cached = findQuotedMessage(peer, remoteId);
        if (cached != null || !productionMode) { completion.accept(cached); return; }
        String owner = chatState.ownerUserId();
        int generation = accountGeneration;
        enqueueIO(() -> {
            RemoteIMMessage stored = historyStore.messageWithRemoteId(owner, clean(peer), clean(remoteId));
            runOnMain(() -> {
                if (generation != accountGeneration) return;
                if (stored != null) chatState.mergeMessages(Collections.singletonList(stored));
                completion.accept(stored);
            });
        });
    }

    private void updateOldestLoadedCursor(String peerId, List<RemoteIMMessage> pageMessages) {
        if (pageMessages == null || pageMessages.isEmpty()) return;
        RemoteIMMessage oldest = pageMessages.get(0);
        recordOldestLoadedCursor(peerId, oldest.createdAtMillis(), oldest.id());
    }

    void recordOldestLoadedCursor(String peerId, long createdAtMillis, String messageId) {
        String cleanPeerId = clean(peerId);
        String cleanMessageId = clean(messageId);
        if (cleanPeerId.isEmpty() || cleanMessageId.isEmpty()) return;
        oldestLoadedCreatedAtByUserId.put(cleanPeerId, createdAtMillis);
        oldestLoadedMessageIdByUserId.put(cleanPeerId, cleanMessageId);
    }

    Long oldestLoadedCreatedAt(String peerId) {
        return oldestLoadedCreatedAtByUserId.get(clean(peerId));
    }

    String oldestLoadedMessageId(String peerId) {
        return oldestLoadedMessageIdByUserId.get(clean(peerId));
    }

    public boolean hasEarlierMessages(String userId) {
        return hasEarlierByUserId.getOrDefault(clean(userId), false);
    }

    public void destroy() {
        if (!productionMode) return;
        resetActivities();
        if (pendingStateNotification != null) {
            mainHandler.removeCallbacks(pendingStateNotification);
            pendingStateNotification = null;
        }
        if (remoteDesktop != null) remoteDesktop.destroy();
        destroyed = true;
        client.destroy();
        ioExecutor.execute(historyStore::close);
        ioExecutor.shutdown();
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
        String owner = chatState.ownerUserId();
        int generation = accountGeneration;
        int peerEpoch = peerEpochs.getOrDefault(incoming.fromUserId(), 0);
        RemoteIMActivitySignal replacedActivity = activities.get(incoming.fromUserId());
        enqueueIO(() -> {
            boolean duplicate = !incoming.remoteId().isEmpty() && historyStore.containsRemoteId(owner, incoming.remoteId());
            runOnMain(() -> {
                if (duplicate || generation != accountGeneration
                    || peerEpoch != peerEpochs.getOrDefault(incoming.fromUserId(), 0)) return;
                if (!incoming.remoteId().isEmpty() && chatState.messageWithRemoteId(incoming.remoteId()) != null) return;
                applyIncomingMessage(incoming, replacedActivity);
            });
        });
    }

    private void applyIncomingMessage(RemoteIMMessage incoming, RemoteIMActivitySignal replacedActivity) {
        RemoteIMVideoAttachment preparedVideo = pendingVideoMedia.remove(incoming.remoteId());
        if (preparedVideo != null && incoming.videoAttachment() != null) incoming = incoming.withVideoAttachment(preparedVideo);
        RemoteIMActivitySignal currentActivity = activities.get(incoming.fromUserId());
        if (replacedActivity != null && currentActivity != null && replacedActivity.activityId.equals(currentActivity.activityId)) {
            activities.clear(incoming.fromUserId());
        }
        boolean knownContact = contactExists(incoming.fromUserId());
        int previousMessageCount = chatState.messages().size();

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
                incoming.origin(),
                incoming.text(),
                incoming.captionAbove()
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
                incoming.origin(),
                incoming.text(),
                incoming.captionAbove()
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
                incoming.origin(),
                incoming.text(),
                incoming.captionAbove()
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
        message.setQuote(incoming.quote());
        boolean wasInserted = chatState.messages().size() > previousMessageCount;
        RemoteIMContact contact = findContact(incoming.fromUserId());
        persistContact(contact);
        persistMessage(message);
        if (!visibleConversationUserId.equals(incoming.fromUserId())) {
            unreadByUserId.put(
                incoming.fromUserId(),
                unreadByUserId.getOrDefault(incoming.fromUserId(), 0) + 1
            );
        }
        if (!visibleConversationUserId.equals(incoming.fromUserId())) chatState.retainRecentMessages(incoming.fromUserId(), 20);
        if (!knownContact) {
            refreshContactMetadata(Collections.singletonList(incoming.fromUserId()));
        }
        if (wasInserted) {
            listener.onNewIncomingMessage(
                message,
                visibleConversationUserId.equals(incoming.fromUserId())
            );
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
        if (productionMode) {
            String owner = chatState.ownerUserId();
            RemoteIMMessage snapshot = message.snapshot();
            enqueueIO(() -> historyStore.upsertMessage(owner, snapshot));
        }
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

    private ChatState readProductionChatState(RemoteIMSettings account) {
        String ownerUserId = account.requiresLogin() ? FALLBACK_OWNER_USER_ID : account.loginUserId();
        if (account.requiresLogin()) return new ChatState(ownerUserId);
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
            if (pendingStateNotification != null) return;
            pendingStateNotification = () -> {
                pendingStateNotification = null;
                listener.onStateChanged();
            };
            mainHandler.post(pendingStateNotification);
        };
        if (Looper.myLooper() == Looper.getMainLooper()) schedule.run();
        else mainHandler.post(schedule);
    }

    private void reportError(String message) {
        if (listener == null) return;
        runOnMain(() -> listener.onError(message == null ? "操作失败" : message));
    }

    private void runOnMain(Runnable runnable) {
        if (destroyed) return;
        if (mainHandler == null || Looper.myLooper() == Looper.getMainLooper()) runnable.run();
        else mainHandler.post(() -> { if (!destroyed) runnable.run(); });
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
                if (state != TencentIMClient.ConnectionState.CONNECTED) {
                    resetActivities();
                    if (listener != null) listener.onActivityChanged(visibleConversationUserId);
                }
                connectionState = state;
                connectionDetail = detail;
                if (state == TencentIMClient.ConnectionState.CONNECTED) refreshContactMetadata();
                notifyStateChanged();
            });
        }

        @Override public void onVideoMediaUpdated(String sender, String recipient, String remoteId, RemoteIMVideoAttachment attachment) {
            runOnMain(() -> {
                if (requiresLogin() || !chatState.ownerUserId().equals(recipient)) return;
                pendingVideoMedia.put(remoteId, attachment);
                if (pendingVideoMedia.size() > 128) pendingVideoMedia.remove(pendingVideoMedia.keySet().iterator().next());
                RemoteIMMessage current = chatState.updateVideoMedia(remoteId, attachment);
                if (current != null) {
                    pendingVideoMedia.remove(remoteId); persistMessage(current);
                } else {
                    enqueueIO(() -> {
                        RemoteIMMessage stored = historyStore.messageWithRemoteId(recipient, sender, remoteId);
                        if (stored != null) historyStore.upsertMessage(recipient, stored.withVideoAttachment(attachment));
                    });
                }
                mediaRevision++; notifyStateChanged();
            });
        }

        @Override
        public void onIncomingActivity(String sender, String recipient, RemoteIMActivitySignal signal) {
            runOnMain(() -> {
                if (requiresLogin() || !chatState.ownerUserId().equals(recipient)
                    || sender.equals(recipient) || !contactExists(sender)) return;
                if (activities.receive(sender, signal, android.os.SystemClock.elapsedRealtime()) && listener != null) {
                    listener.onActivityChanged(sender);
                }
                scheduleActivityExpiry();
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
                    persistContact(contact);
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
