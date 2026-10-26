package com.kongshang.maichat;

import java.util.Objects;

public final class RemoteIMGroup {
    private final String groupId;
    private final String name;
    private final int memberCount;
    private final boolean pendingActivation;

    public RemoteIMGroup(String groupId, String name, int memberCount) {
        this(groupId, name, memberCount, false);
    }

    public RemoteIMGroup(String groupId, String name, int memberCount, boolean pendingActivation) {
        this.groupId = Objects.requireNonNull(groupId).trim();
        if (this.groupId.isEmpty()) throw new IllegalArgumentException("groupId is required");
        String cleanedName = name == null ? "" : name.trim();
        this.name = cleanedName.isEmpty() ? this.groupId : cleanedName;
        this.memberCount = Math.max(0, memberCount);
        this.pendingActivation = pendingActivation;
    }

    public String groupId() { return groupId; }
    public String name() { return name; }
    public int memberCount() { return memberCount; }
    public boolean pendingActivation() { return pendingActivation; }
    public RemoteIMGroup withPendingActivation(boolean pending) {
        return new RemoteIMGroup(groupId,name,memberCount,pending);
    }
}
