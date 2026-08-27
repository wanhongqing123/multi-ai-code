package com.kongshang.maichat;

import java.util.ArrayList;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Objects;
import java.util.regex.Pattern;

public final class RemoteIMApprovalRequest {
    private static final Pattern TOKEN_PATTERN = Pattern.compile(
        "^approval-[A-Za-z0-9_-]{1,191}$"
    );

    private final String token;
    private final List<RemoteIMApprovalAction> actions;

    public RemoteIMApprovalRequest(String token, List<RemoteIMApprovalAction> actions) {
        String cleanToken = clean(token);
        List<RemoteIMApprovalAction> cleanActions = actions == null
            ? Collections.emptyList()
            : new ArrayList<>(actions);
        if (!isValidToken(cleanToken)
            || cleanActions.size() < 2
            || cleanActions.size() > 3
            || cleanActions.contains(null)
            || cleanActions.contains(RemoteIMApprovalAction.RESOLVED)
            || cleanActions.contains(RemoteIMApprovalAction.AUTO_DECLINED)
            || new HashSet<>(cleanActions).size() != cleanActions.size()
            || !cleanActions.contains(RemoteIMApprovalAction.APPROVE_ONCE)
            || !cleanActions.contains(RemoteIMApprovalAction.REJECT)) {
            throw new IllegalArgumentException("invalid approval request");
        }
        this.token = cleanToken;
        this.actions = Collections.unmodifiableList(cleanActions);
    }

    public String token() {
        return token;
    }

    public List<RemoteIMApprovalAction> actions() {
        return actions;
    }

    public boolean allows(RemoteIMApprovalAction action) {
        return actions.contains(action);
    }

    public static boolean isValidToken(String token) {
        return token != null && TOKEN_PATTERN.matcher(token.trim()).matches();
    }

    private static String clean(String value) {
        return value == null ? "" : value.trim();
    }

    @Override
    public boolean equals(Object other) {
        if (this == other) return true;
        if (!(other instanceof RemoteIMApprovalRequest)) return false;
        RemoteIMApprovalRequest that = (RemoteIMApprovalRequest) other;
        return token.equals(that.token) && actions.equals(that.actions);
    }

    @Override
    public int hashCode() {
        return Objects.hash(token, actions);
    }
}
