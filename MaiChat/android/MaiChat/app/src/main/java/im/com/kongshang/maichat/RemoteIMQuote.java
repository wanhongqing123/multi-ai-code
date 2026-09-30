package com.kongshang.maichat;

import java.util.Objects;

public final class RemoteIMQuote {
    private final String messageId;
    private final String senderId;
    private final String digest;
    private final String kind;

    public RemoteIMQuote(String messageId, String senderId, String digest, String kind) {
        this.messageId = clean(messageId);
        this.senderId = clean(senderId);
        this.digest = clean(digest);
        this.kind = clean(kind);
        if (this.digest.isEmpty()) throw new IllegalArgumentException("quote digest is required");
    }

    public String messageId() { return messageId; }
    public String senderId() { return senderId; }
    public String digest() { return digest; }
    public String kind() { return kind; }

    private static String clean(String value) {
        return value == null ? "" : value.trim();
    }

    @Override
    public boolean equals(Object other) {
        if (this == other) return true;
        if (!(other instanceof RemoteIMQuote)) return false;
        RemoteIMQuote that = (RemoteIMQuote) other;
        return messageId.equals(that.messageId)
            && senderId.equals(that.senderId)
            && digest.equals(that.digest)
            && kind.equals(that.kind);
    }

    @Override
    public int hashCode() {
        return Objects.hash(messageId, senderId, digest, kind);
    }
}
