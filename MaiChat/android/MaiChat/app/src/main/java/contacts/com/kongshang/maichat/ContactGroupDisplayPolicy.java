package com.kongshang.maichat;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Locale;
import java.util.Set;

public final class ContactGroupDisplayPolicy {
    public enum Kind { GROUP_HEADER, CONTACT }

    public static final class Row {
        private final Kind kind;
        private final String groupName;
        private final RemoteIMContact contact;
        private final int memberCount;

        private Row(Kind kind, String groupName, RemoteIMContact contact, int memberCount) {
            this.kind = kind;
            this.groupName = groupName;
            this.contact = contact;
            this.memberCount = memberCount;
        }

        public Kind kind() { return kind; }
        public String groupName() { return groupName; }
        public RemoteIMContact contact() { return contact; }
        public int memberCount() { return memberCount; }
        public boolean isIndented() { return kind == Kind.CONTACT && !groupName.isEmpty(); }
    }

    private ContactGroupDisplayPolicy() {}

    public static List<Row> rows(
        List<String> groups,
        List<RemoteIMContact> contacts,
        Set<String> collapsedGroups,
        String queryValue
    ) {
        List<String> safeGroups = groups == null ? Collections.emptyList() : groups;
        List<RemoteIMContact> safeContacts = contacts == null
            ? Collections.emptyList()
            : contacts;
        Set<String> safeCollapsed = collapsedGroups == null
            ? Collections.emptySet()
            : collapsedGroups;
        String query = queryValue == null ? "" : queryValue.trim().toLowerCase(Locale.ROOT);
        boolean searching = !query.isEmpty();
        List<Row> result = new ArrayList<>();

        for (String group : safeGroups) {
            List<RemoteIMContact> allMembers = new ArrayList<>();
            List<RemoteIMContact> matchedMembers = new ArrayList<>();
            for (RemoteIMContact contact : safeContacts) {
                if (!group.equals(contact.groupName())) continue;
                allMembers.add(contact);
                if (matches(contact, query)) matchedMembers.add(contact);
            }
            if (searching && matchedMembers.isEmpty()) continue;
            result.add(new Row(Kind.GROUP_HEADER, group, null, allMembers.size()));
            if (searching || !safeCollapsed.contains(group)) {
                for (RemoteIMContact contact : matchedMembers) {
                    result.add(new Row(Kind.CONTACT, group, contact, 0));
                }
            }
        }

        // 无分组联系人直接与组表头同层排在最后，不创建「未分组」表头。
        for (RemoteIMContact contact : safeContacts) {
            if (contact.groupName().isEmpty() && matches(contact, query)) {
                result.add(new Row(Kind.CONTACT, "", contact, 0));
            }
        }
        return Collections.unmodifiableList(result);
    }

    private static boolean matches(RemoteIMContact contact, String query) {
        return query.isEmpty()
            || contact.userId().toLowerCase(Locale.ROOT).contains(query)
            || contact.displayName().toLowerCase(Locale.ROOT).contains(query);
    }
}
