package com.kongshang.maichat;

import java.util.Objects;

public final class RemoteIMSettings {
    private final String loginUserId;

    public RemoteIMSettings(String loginUserId) {
        this.loginUserId = loginUserId == null ? "" : loginUserId.trim();
    }

    public static RemoteIMSettings empty() {
        return new RemoteIMSettings("");
    }

    public String loginUserId() {
        return loginUserId;
    }

    public boolean requiresLogin() {
        return loginUserId.isEmpty();
    }

    @Override
    public boolean equals(Object other) {
        if (this == other) return true;
        if (!(other instanceof RemoteIMSettings)) return false;
        RemoteIMSettings settings = (RemoteIMSettings) other;
        return loginUserId.equals(settings.loginUserId);
    }

    @Override
    public int hashCode() {
        return Objects.hash(loginUserId);
    }
}
