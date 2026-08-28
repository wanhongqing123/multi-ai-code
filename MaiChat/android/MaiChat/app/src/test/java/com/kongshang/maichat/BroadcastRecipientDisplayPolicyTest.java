package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;

import org.junit.Test;

import java.util.HashSet;
import java.util.List;
import java.util.Set;

public class BroadcastRecipientDisplayPolicyTest {
    @Test
    public void filteringChangesOnlyVisibleRowsAndKeepsGroupTotals() {
        List<RemoteIMContact> contacts = List.of(
            new RemoteIMContact("alice", "Alice", "", "同事"),
            new RemoteIMContact("amy", "Amy", "", "同事"),
            new RemoteIMContact("bob", "Bob")
        );
        Set<String> selected = new HashSet<>(Set.of("alice", "amy"));

        List<BroadcastRecipientDisplayPolicy.Row> rows =
            BroadcastRecipientDisplayPolicy.rows(List.of("同事"), contacts, "ali");

        assertEquals(Set.of("alice", "amy"), selected);
        assertEquals(2, rows.size());
        assertEquals(BroadcastRecipientDisplayPolicy.Kind.GROUP_HEADER, rows.get(0).kind());
        assertEquals(2, rows.get(0).memberCount());
        assertEquals("alice", rows.get(1).contact().userId());
        assertFalse(rows.get(1).contact().userId().equals("amy"));
    }
}
