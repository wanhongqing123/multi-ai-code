package com.kongshang.maichat;

import java.util.UUID;

public final class RemoteIMGroupMessage {
    public enum Status { PENDING, SENT, RECEIVED, FAILED }

    private final String id;
    private String remoteId;
    private final String groupId;
    private final String senderId;
    private final String text;
    private final long createdAtMillis;
    private Status status;

    public RemoteIMGroupMessage(String id, String remoteId, String groupId, String senderId,
                                String text, long createdAtMillis, Status status) {
        this.id = id == null || id.isEmpty()
            ? remoteId != null && !remoteId.isEmpty()
                ? "group:" + groupId + ":" + remoteId
                : UUID.randomUUID().toString()
            : id;
        this.remoteId = remoteId == null ? "" : remoteId;
        this.groupId = groupId;
        this.senderId = senderId;
        this.text = text;
        this.createdAtMillis = createdAtMillis;
        this.status = status;
    }

    public String id() { return id; }
    public String remoteId() { return remoteId; }
    public String groupId() { return groupId; }
    public String senderId() { return senderId; }
    public String text() { return text; }
    public long createdAtMillis() { return createdAtMillis; }
    public Status status() { return status; }
    public void updateDelivery(String remoteId, Status status) {
        if (remoteId != null && !remoteId.isEmpty()) this.remoteId = remoteId;
        this.status = status;
    }
}
