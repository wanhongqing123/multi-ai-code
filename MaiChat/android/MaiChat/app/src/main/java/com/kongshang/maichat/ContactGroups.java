package com.kongshang.maichat;

public final class ContactGroups {
    private ContactGroups() {}

    public static String normalize(String value) {
        return value == null ? "" : value.trim();
    }

    public static boolean isAcceptableName(String normalized) {
        return normalized != null && !normalized.isEmpty();
    }
}
