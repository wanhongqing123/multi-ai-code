package com.kongshang.maichat;

import java.util.Objects;

public final class RemoteIMContact {
    private final String userId;
    private final String displayName;
    private final String avatarUrl;

    public RemoteIMContact(String userId, String displayName) {
        this(userId, displayName, "");
    }

    public RemoteIMContact(String userId, String displayName, String avatarUrl) {
        this.userId = clean(userId);
        this.displayName = clean(displayName).isEmpty() ? this.userId : clean(displayName);
        this.avatarUrl = clean(avatarUrl);
        if (this.userId.isEmpty()) {
            throw new IllegalArgumentException("userId is required");
        }
    }

    public String userId() {
        return userId;
    }

    public String displayName() {
        return displayName;
    }

    public String avatarUrl() {
        return avatarUrl;
    }

    private static String clean(String value) {
        return value == null ? "" : value.trim();
    }

    @Override
    public boolean equals(Object other) {
        if (this == other) return true;
        if (!(other instanceof RemoteIMContact)) return false;
        RemoteIMContact that = (RemoteIMContact) other;
        return userId.equals(that.userId)
            && displayName.equals(that.displayName)
            && avatarUrl.equals(that.avatarUrl);
    }

    @Override
    public int hashCode() {
        return Objects.hash(userId, displayName, avatarUrl);
    }
}
