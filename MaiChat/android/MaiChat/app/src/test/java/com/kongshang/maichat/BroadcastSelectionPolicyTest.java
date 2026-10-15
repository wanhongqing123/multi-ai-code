// 文件作用：Android 本地测试；BroadcastSelectionPolicyTest 负责覆盖对应类的状态和数据行为，本文件给出其验证。
// 运行边界：仅供自动化验证使用，不作为正式 App 或 Agent 的用户功能入口。
// 覆盖场景：deduplicatesRecipientsAndDropsBlankIds、groupHeadersExposeNonePartialAndAllWithoutCountingUngroupedContacts。
package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;

import org.junit.Test;

import java.util.HashSet;
import java.util.List;
import java.util.Set;

public class BroadcastSelectionPolicyTest {
    private final List<RemoteIMContact> contacts = List.of(
        new RemoteIMContact("alice", "Alice", "", "同事"),
        new RemoteIMContact("amy", "Amy", "", "同事"),
        new RemoteIMContact("bob", "Bob")
    );

    @Test
    public void deduplicatesRecipientsAndDropsBlankIds() {
        assertEquals(
            List.of("alice", "bob"),
            BroadcastSelectionPolicy.uniqueRecipientIds(
                List.of(" alice ", "", "bob", "alice", "  ")
            )
        );
    }

    @Test
    public void groupHeadersExposeNonePartialAndAllWithoutCountingUngroupedContacts() {
        Set<String> selected = new HashSet<>();
        assertEquals(
            BroadcastSelectionPolicy.GroupState.NONE,
            BroadcastSelectionPolicy.groupState("同事", contacts, selected)
        );
        selected.add("alice");
        selected.add("bob");
        assertEquals(
            BroadcastSelectionPolicy.GroupState.PARTIAL,
            BroadcastSelectionPolicy.groupState("同事", contacts, selected)
        );
        BroadcastSelectionPolicy.setGroupSelected("同事", contacts, selected, true);
        assertEquals(
            BroadcastSelectionPolicy.GroupState.ALL,
            BroadcastSelectionPolicy.groupState("同事", contacts, selected)
        );
        assertEquals(Set.of("alice", "amy", "bob"), selected);
    }
}
