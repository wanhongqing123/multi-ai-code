package com.kongshang.maichat;

import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;

public final class BroadcastRecipientPickerState {
    private final Set<String> selectedUserIds = new LinkedHashSet<>();
    private String filterText = "";

    public void setFilterText(String value) {
        // 筛选只改可见行，绝不碰 selectedUserIds。
        filterText = value == null ? "" : value;
    }

    public String filterText() { return filterText; }

    public Set<String> selectedUserIds() {
        return new LinkedHashSet<>(selectedUserIds);
    }

    public boolean isSelected(String userId) { return selectedUserIds.contains(userId); }

    public void toggleContact(String userId) {
        if (!selectedUserIds.add(userId)) selectedUserIds.remove(userId);
    }

    public BroadcastSelectionPolicy.GroupState groupState(
        String groupName,
        List<RemoteIMContact> contacts
    ) {
        return BroadcastSelectionPolicy.groupState(groupName, contacts, selectedUserIds);
    }

    public void toggleGroup(String groupName, List<RemoteIMContact> contacts) {
        BroadcastSelectionPolicy.GroupState state = groupState(groupName, contacts);
        BroadcastSelectionPolicy.setGroupSelected(
            groupName,
            contacts,
            selectedUserIds,
            state != BroadcastSelectionPolicy.GroupState.ALL
        );
    }

    public List<BroadcastRecipientDisplayPolicy.Row> visibleRows(
        List<String> groups,
        List<RemoteIMContact> contacts
    ) {
        return BroadcastRecipientDisplayPolicy.rows(groups, contacts, filterText);
    }
}
