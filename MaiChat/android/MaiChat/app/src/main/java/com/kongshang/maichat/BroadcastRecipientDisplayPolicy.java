package com.kongshang.maichat;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Locale;

public final class BroadcastRecipientDisplayPolicy {
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

    private BroadcastRecipientDisplayPolicy() {}

    public static List<Row> rows(
        List<String> groups,
        List<RemoteIMContact> contacts,
        String queryValue
    ) {
        String query = queryValue == null ? "" : queryValue.trim().toLowerCase(Locale.ROOT);
        List<Row> result = new ArrayList<>();
        for (String group : groups) {
            List<RemoteIMContact> allMembers = new ArrayList<>();
            List<RemoteIMContact> visibleMembers = new ArrayList<>();
            boolean groupMatches = group.toLowerCase(Locale.ROOT).contains(query);
            for (RemoteIMContact contact : contacts) {
                if (!group.equals(contact.groupName())) continue;
                allMembers.add(contact);
                if (query.isEmpty() || groupMatches || matches(contact, query)) {
                    visibleMembers.add(contact);
                }
            }
            if (!query.isEmpty() && !groupMatches && visibleMembers.isEmpty()) continue;
            result.add(new Row(Kind.GROUP_HEADER, group, null, allMembers.size()));
            for (RemoteIMContact contact : visibleMembers) {
                result.add(new Row(Kind.CONTACT, group, contact, 0));
            }
        }
        for (RemoteIMContact contact : contacts) {
            if (contact.groupName().isEmpty() && (query.isEmpty() || matches(contact, query))) {
                result.add(new Row(Kind.CONTACT, "", contact, 0));
            }
        }
        return Collections.unmodifiableList(result);
    }

    private static boolean matches(RemoteIMContact contact, String query) {
        return contact.displayName().toLowerCase(Locale.ROOT).contains(query)
            || contact.userId().toLowerCase(Locale.ROOT).contains(query);
    }
}
