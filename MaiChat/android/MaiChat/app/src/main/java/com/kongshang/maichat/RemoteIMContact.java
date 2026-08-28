package com.kongshang.maichat;

import java.util.Objects;

public final class RemoteIMContact {
    private final String userId;
    private final String displayName;
    private final String avatarUrl;
    private final String groupName;

    public RemoteIMContact(String userId, String displayName) {
        this(userId, displayName, "", "");
    }

    public RemoteIMContact(String userId, String displayName, String avatarUrl) {
        this(userId, displayName, avatarUrl, "");
    }

    public RemoteIMContact(String userId, String displayName, String avatarUrl, String groupName) {
        this.userId = clean(userId);
        this.displayName = clean(displayName).isEmpty() ? this.userId : clean(displayName);
        this.avatarUrl = clean(avatarUrl);
        this.groupName = ContactGroups.normalize(groupName);
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

    public String groupName() {
        return groupName;
    }

    public RemoteIMContact withGroupName(String value) {
        return new RemoteIMContact(userId, displayName, avatarUrl, value);
    }

    public RemoteIMContact withProfile(String nextDisplayName, String nextAvatarUrl) {
        String cleanDisplayName = clean(nextDisplayName);
        String cleanAvatarUrl = clean(nextAvatarUrl);
        String resolvedDisplayName = cleanDisplayName.isEmpty()
            || (cleanDisplayName.equals(userId)
                && !displayName.isEmpty()
                && !displayName.equals(userId))
            ? displayName
            : cleanDisplayName;
        String resolvedAvatarUrl = cleanAvatarUrl.isEmpty() ? avatarUrl : cleanAvatarUrl;
        return new RemoteIMContact(userId, resolvedDisplayName, resolvedAvatarUrl, groupName);
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
            && avatarUrl.equals(that.avatarUrl)
            && groupName.equals(that.groupName);
    }

    @Override
    public int hashCode() {
        return Objects.hash(userId, displayName, avatarUrl, groupName);
    }
}
