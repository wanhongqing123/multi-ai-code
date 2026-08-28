package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

import java.util.List;
import java.util.Set;

public class ContactGroupDisplayPolicyTest {
    @Test
    public void keepsEmptyGroupsAndRendersUngroupedContactsWithoutAHeader() {
        List<ContactGroupDisplayPolicy.Row> rows = ContactGroupDisplayPolicy.rows(
            List.of("同事", "空组"),
            List.of(
                new RemoteIMContact("alice", "Alice", "", "同事"),
                new RemoteIMContact("bob", "Bob")
            ),
            Set.of(),
            ""
        );

        assertEquals(4, rows.size());
        assertEquals(ContactGroupDisplayPolicy.Kind.GROUP_HEADER, rows.get(0).kind());
        assertEquals(1, rows.get(0).memberCount());
        assertTrue(rows.get(1).isIndented());
        assertEquals("空组", rows.get(2).groupName());
        assertEquals(0, rows.get(2).memberCount());
        assertEquals(ContactGroupDisplayPolicy.Kind.CONTACT, rows.get(3).kind());
        assertFalse(rows.get(3).isIndented());
    }

    @Test
    public void searchIgnoresCollapseAndHidesGroupsWithoutMatches() {
        List<ContactGroupDisplayPolicy.Row> rows = ContactGroupDisplayPolicy.rows(
            List.of("同事", "家人"),
            List.of(
                new RemoteIMContact("alice", "Alice", "", "同事"),
                new RemoteIMContact("bob", "Bob", "", "家人"),
                new RemoteIMContact("charlie", "Charlie")
            ),
            Set.of("同事"),
            "ali"
        );

        assertEquals(2, rows.size());
        assertEquals("同事", rows.get(0).groupName());
        assertEquals("alice", rows.get(1).contact().userId());
    }
}
