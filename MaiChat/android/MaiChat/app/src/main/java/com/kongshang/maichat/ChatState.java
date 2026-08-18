package com.kongshang.maichat;

import java.io.File;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;

public final class ChatState {
    private final String ownerUserId;
    private final List<RemoteIMContact> contacts = new ArrayList<>();
    private final List<RemoteIMMessage> messages = new ArrayList<>();
    private String selectedPeerId;

    public ChatState(String ownerUserId) {
        this.ownerUserId = clean(ownerUserId);
        if (this.ownerUserId.isEmpty()) {
            throw new IllegalArgumentException("ownerUserId is required");
        }
    }

    public String ownerUserId() {
        return ownerUserId;
    }

    public String selectedPeerId() {
        return selectedPeerId;
    }

    public List<RemoteIMContact> contacts() {
        return Collections.unmodifiableList(new ArrayList<>(contacts));
    }

    public List<RemoteIMMessage> messages() {
        return Collections.unmodifiableList(new ArrayList<>(messages));
    }

    public void upsertContact(RemoteIMContact contact) {
        for (int index = 0; index < contacts.size(); index += 1) {
            if (contacts.get(index).userId().equals(contact.userId())) {
                contacts.set(index, contact);
                return;
            }
        }
        contacts.add(contact);
    }

    public boolean removeContact(String userId) {
        String cleanUserId = clean(userId);
        boolean removed = contacts.removeIf(contact -> contact.userId().equals(cleanUserId));
        removeMessagesWith(cleanUserId);
        if (cleanUserId.equals(selectedPeerId)) {
            selectedPeerId = contacts.isEmpty() ? null : contacts.get(0).userId();
        }
        return removed;
    }

    public void removeMessagesWith(String peerId) {
        String cleanPeerId = clean(peerId);
        messages.removeIf(message ->
            message.fromUserId().equals(cleanPeerId) || message.toUserId().equals(cleanPeerId)
        );
    }

    public void selectPeer(String userId) {
        String cleanUserId = clean(userId);
        if (cleanUserId.isEmpty()) {
            selectedPeerId = null;
            return;
        }
        selectedPeerId = cleanUserId;
    }

    public RemoteIMMessage queueOutgoingText(String text) {
        String cleanText = clean(text);
        if (cleanText.isEmpty()) {
            throw new IllegalArgumentException("text is required");
        }
        String peerId = requireSelectedPeer();
        RemoteIMMessage message = new RemoteIMMessage(
            ownerUserId,
            peerId,
            cleanText,
            RemoteIMMessage.Direction.OUTGOING,
            RemoteIMMessage.Status.PENDING,
            System.currentTimeMillis(),
            null,
            null
        );
        messages.add(message);
        return message;
    }

    public RemoteIMMessage queueOutgoingImage(String localPath, int width, int height, long sizeBytes) {
        String cleanPath = clean(localPath);
        if (cleanPath.isEmpty()) {
            throw new IllegalArgumentException("localPath is required");
        }
        String peerId = requireSelectedPeer();
        RemoteIMImageAttachment attachment = new RemoteIMImageAttachment(
            cleanPath,
            width,
            height,
            sizeBytes
        );
        RemoteIMMessage message = new RemoteIMMessage(
            ownerUserId,
            peerId,
            "[图片消息] " + fileName(cleanPath),
            RemoteIMMessage.Direction.OUTGOING,
            RemoteIMMessage.Status.PENDING,
            System.currentTimeMillis(),
            attachment,
            null
        );
        messages.add(message);
        return message;
    }

    public RemoteIMMessage queueOutgoingVoice(String localPath, int durationSeconds) {
        String cleanPath = clean(localPath);
        if (cleanPath.isEmpty()) {
            throw new IllegalArgumentException("localPath is required");
        }
        String peerId = requireSelectedPeer();
        RemoteIMVoiceAttachment attachment = new RemoteIMVoiceAttachment(cleanPath, durationSeconds);
        RemoteIMMessage message = new RemoteIMMessage(
            ownerUserId,
            peerId,
            "[语音消息 " + attachment.durationSeconds() + "s]",
            RemoteIMMessage.Direction.OUTGOING,
            RemoteIMMessage.Status.PENDING,
            System.currentTimeMillis(),
            null,
            attachment
        );
        messages.add(message);
        return message;
    }

    public RemoteIMMessage queueOutgoingFile(String localPath, String fileName, String mimeType, long sizeBytes) {
        String cleanPath = clean(localPath);
        if (cleanPath.isEmpty()) {
            throw new IllegalArgumentException("localPath is required");
        }
        String peerId = requireSelectedPeer();
        RemoteIMFileAttachment attachment = new RemoteIMFileAttachment(
            cleanPath,
            fileName,
            mimeType,
            sizeBytes
        );
        RemoteIMMessage message = new RemoteIMMessage(
            ownerUserId,
            peerId,
            fileDisplayText(attachment.fileName(), cleanPath),
            RemoteIMMessage.Direction.OUTGOING,
            RemoteIMMessage.Status.PENDING,
            System.currentTimeMillis(),
            null,
            null,
            attachment
        );
        messages.add(message);
        return message;
    }

    public RemoteIMMessage receiveText(String text, String fromUserId) {
        return receiveText(text, fromUserId, null, System.currentTimeMillis(), RemoteIMOrigin.MACHINE);
    }

    public RemoteIMMessage receiveText(
        String text,
        String fromUserId,
        String remoteId,
        long createdAtMillis,
        RemoteIMOrigin origin
    ) {
        RemoteIMMessage existing = messageWithRemoteId(remoteId);
        if (existing != null) return existing;
        String peerId = clean(fromUserId);
        upsertContact(new RemoteIMContact(peerId, peerId));
        RemoteIMMessage message = new RemoteIMMessage(
            null,
            remoteId,
            peerId,
            ownerUserId,
            incomingDisplayText(text),
            RemoteIMMessage.Direction.INCOMING,
            RemoteIMMessage.Status.RECEIVED,
            createdAtMillis,
            null,
            null,
            null,
            origin
        );
        messages.add(message);
        return message;
    }

    public RemoteIMMessage receiveImage(
        String localPath,
        String fromUserId,
        int width,
        int height,
        long sizeBytes
    ) {
        return receiveImage(
            localPath,
            fromUserId,
            width,
            height,
            sizeBytes,
            null,
            System.currentTimeMillis(),
            RemoteIMOrigin.MACHINE
        );
    }

    public RemoteIMMessage receiveImage(
        String localPath,
        String fromUserId,
        int width,
        int height,
        long sizeBytes,
        String remoteId,
        long createdAtMillis,
        RemoteIMOrigin origin
    ) {
        RemoteIMMessage existing = messageWithRemoteId(remoteId);
        if (existing != null) return existing;
        String peerId = clean(fromUserId);
        String cleanPath = clean(localPath);
        upsertContact(new RemoteIMContact(peerId, peerId));
        RemoteIMImageAttachment attachment = new RemoteIMImageAttachment(
            cleanPath,
            width,
            height,
            sizeBytes
        );
        RemoteIMMessage message = new RemoteIMMessage(
            null,
            remoteId,
            peerId,
            ownerUserId,
            "[图片消息] " + fileName(cleanPath),
            RemoteIMMessage.Direction.INCOMING,
            RemoteIMMessage.Status.RECEIVED,
            createdAtMillis,
            attachment,
            null,
            null,
            origin
        );
        messages.add(message);
        return message;
    }

    public RemoteIMMessage receiveVoice(String localPath, int durationSeconds, String fromUserId) {
        return receiveVoice(
            localPath,
            durationSeconds,
            fromUserId,
            null,
            System.currentTimeMillis(),
            RemoteIMOrigin.MACHINE
        );
    }

    public RemoteIMMessage receiveVoice(
        String localPath,
        int durationSeconds,
        String fromUserId,
        String remoteId,
        long createdAtMillis,
        RemoteIMOrigin origin
    ) {
        RemoteIMMessage existing = messageWithRemoteId(remoteId);
        if (existing != null) return existing;
        String peerId = clean(fromUserId);
        upsertContact(new RemoteIMContact(peerId, peerId));
        RemoteIMVoiceAttachment attachment = new RemoteIMVoiceAttachment(localPath, durationSeconds);
        RemoteIMMessage message = new RemoteIMMessage(
            null,
            remoteId,
            peerId,
            ownerUserId,
            "[语音消息 " + attachment.durationSeconds() + "s]",
            RemoteIMMessage.Direction.INCOMING,
            RemoteIMMessage.Status.RECEIVED,
            createdAtMillis,
            null,
            attachment,
            null,
            origin
        );
        messages.add(message);
        return message;
    }

    public RemoteIMMessage receiveFile(
        String localPath,
        String fromUserId,
        String fileName,
        String mimeType,
        long sizeBytes
    ) {
        return receiveFile(
            localPath,
            fromUserId,
            fileName,
            mimeType,
            sizeBytes,
            null,
            System.currentTimeMillis(),
            RemoteIMOrigin.MACHINE
        );
    }

    public RemoteIMMessage receiveFile(
        String localPath,
        String fromUserId,
        String fileName,
        String mimeType,
        long sizeBytes,
        String remoteId,
        long createdAtMillis,
        RemoteIMOrigin origin
    ) {
        RemoteIMMessage existing = messageWithRemoteId(remoteId);
        if (existing != null) return existing;
        String peerId = clean(fromUserId);
        upsertContact(new RemoteIMContact(peerId, peerId));
        RemoteIMFileAttachment attachment = new RemoteIMFileAttachment(
            clean(localPath),
            fileName,
            mimeType,
            sizeBytes
        );
        RemoteIMMessage message = new RemoteIMMessage(
            null,
            remoteId,
            peerId,
            ownerUserId,
            fileDisplayText(attachment.fileName(), attachment.localPath()),
            RemoteIMMessage.Direction.INCOMING,
            RemoteIMMessage.Status.RECEIVED,
            createdAtMillis,
            null,
            null,
            attachment,
            origin
        );
        messages.add(message);
        return message;
    }

    public List<RemoteIMMessage> messagesWith(String peerId) {
        String cleanPeerId = clean(peerId);
        List<RemoteIMMessage> result = new ArrayList<>();
        for (RemoteIMMessage message : messages) {
            if (message.fromUserId().equals(cleanPeerId) || message.toUserId().equals(cleanPeerId)) {
                result.add(message);
            }
        }
        result.sort(Comparator.comparingLong(RemoteIMMessage::createdAtMillis));
        return Collections.unmodifiableList(result);
    }

    public List<RemoteIMMessage> recentMessagesWith(String peerId, int limit) {
        List<RemoteIMMessage> all = messagesWith(peerId);
        int safeLimit = Math.max(1, limit);
        int start = Math.max(0, all.size() - safeLimit);
        return Collections.unmodifiableList(new ArrayList<>(all.subList(start, all.size())));
    }

    public boolean updateMessageStatus(String messageId, RemoteIMMessage.Status status) {
        for (RemoteIMMessage message : messages) {
            if (message.id().equals(messageId)) {
                message.setStatus(status);
                return true;
            }
        }
        return false;
    }

    public boolean updateMessageDelivery(String messageId, String remoteId) {
        for (RemoteIMMessage message : messages) {
            if (message.id().equals(messageId)) {
                message.setStatus(RemoteIMMessage.Status.SENT);
                message.setRemoteId(remoteId);
                return true;
            }
        }
        return false;
    }

    void addRestoredMessage(RemoteIMMessage message) {
        if (message == null || messageWithRemoteId(message.remoteId()) != null) return;
        for (RemoteIMMessage existing : messages) {
            if (existing.id().equals(message.id())) return;
        }
        messages.add(message);
    }

    public void mergeMessages(List<RemoteIMMessage> incoming) {
        if (incoming == null) return;
        for (RemoteIMMessage message : incoming) addRestoredMessage(message);
        messages.sort(Comparator.comparingLong(RemoteIMMessage::createdAtMillis));
    }

    public RemoteIMMessage messageWithRemoteId(String remoteId) {
        String cleanRemoteId = clean(remoteId);
        if (cleanRemoteId.isEmpty()) return null;
        for (RemoteIMMessage message : messages) {
            if (message.remoteId().equals(cleanRemoteId)) return message;
        }
        return null;
    }

    private String requireSelectedPeer() {
        if (selectedPeerId == null || selectedPeerId.isEmpty()) {
            throw new IllegalStateException("selected peer is required");
        }
        return selectedPeerId;
    }

    private static String clean(String value) {
        return value == null ? "" : value.trim();
    }

    private static String incomingDisplayText(String value) {
        String cleanText = clean(value);
        String invisibleAicliPrefix = "\u2063\u200B\u200C\u200D\u2063";
        if (cleanText.startsWith(invisibleAicliPrefix)) {
            return clean(cleanText.substring(invisibleAicliPrefix.length()));
        }
        String[] legacyPrefixes = new String[]{
            "【AICLI 输出】",
            "[AICLI 输出]",
            "【AICLI输出】",
            "[AICLI输出]"
        };
        for (String prefix : legacyPrefixes) {
            if (cleanText.startsWith(prefix)) {
                return clean(cleanText.substring(prefix.length()));
            }
        }
        return cleanText;
    }

    private static String fileName(String path) {
        String name = new File(path).getName();
        return name.isEmpty() ? "image" : name;
    }

    private static String fileDisplayText(String fileName, String path) {
        String cleanName = clean(fileName);
        if (cleanName.isEmpty()) {
            cleanName = new File(path).getName();
        }
        return cleanName.isEmpty() ? "[文件消息]" : "[文件消息] " + cleanName;
    }
}
