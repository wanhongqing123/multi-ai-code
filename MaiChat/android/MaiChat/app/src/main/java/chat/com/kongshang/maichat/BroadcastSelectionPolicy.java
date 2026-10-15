// 文件作用：Android 聊天会话的消息呈现与输入；BroadcastSelectionPolicy 负责输入筛选、权限判断或展示规则，本文件给出其实现。
// 主要入口：uniqueRecipientIds。
package com.kongshang.maichat;

import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;

public final class BroadcastSelectionPolicy {
    public enum GroupState { NONE, PARTIAL, ALL }

    private BroadcastSelectionPolicy() {}

    public static List<String> uniqueRecipientIds(Iterable<String> rawIds) {
        LinkedHashSet<String> unique = new LinkedHashSet<>();
        if (rawIds != null) {
            for (String rawId : rawIds) {
                String clean = rawId == null ? "" : rawId.trim();
                if (!clean.isEmpty()) unique.add(clean);
            }
        }
        return new ArrayList<>(unique);
    }

    public static GroupState groupState(
        String groupName,
        List<RemoteIMContact> contacts,
        Set<String> selectedUserIds
    ) {
        int total = 0;
        int selected = 0;
        for (RemoteIMContact contact : contacts) {
            if (!groupName.equals(contact.groupName())) continue;
            total += 1;
            if (selectedUserIds.contains(contact.userId())) selected += 1;
        }
        if (total == 0 || selected == 0) return GroupState.NONE;
        return selected == total ? GroupState.ALL : GroupState.PARTIAL;
    }

    public static void setGroupSelected(
        String groupName,
        List<RemoteIMContact> contacts,
        Set<String> selectedUserIds,
        boolean selected
    ) {
        for (RemoteIMContact contact : contacts) {
            if (!groupName.equals(contact.groupName())) continue;
            if (selected) selectedUserIds.add(contact.userId());
            else selectedUserIds.remove(contact.userId());
        }
    }
}
