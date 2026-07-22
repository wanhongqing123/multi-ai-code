package com.kongshang.maichat;

import java.util.Objects;

public final class RemoteIMContact {
    private final String userId;
    private final String displayName;

    public RemoteIMContact(String userId, String displayName) {
        this.userId = clean(userId);
        this.displayName = clean(displayName).isEmpty() ? this.userId : clean(displayName);
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

    private static String clean(String value) {
        return value == null ? "" : value.trim();
    }

    @Override
    public boolean equals(Object other) {
        if (this == other) return true;
        if (!(other instanceof RemoteIMContact)) return false;
        RemoteIMContact that = (RemoteIMContact) other;
        return userId.equals(that.userId) && displayName.equals(that.displayName);
    }

    @Override
    public int hashCode() {
        return Objects.hash(userId, displayName);
    }
}
