package com.kongshang.maichat;

import java.util.Objects;

public final class RemoteIMApprovalDecision {
    private final String token;
    private final RemoteIMApprovalAction action;

    public RemoteIMApprovalDecision(String token, RemoteIMApprovalAction action) {
        String cleanToken = token == null ? "" : token.trim();
        if (!RemoteIMApprovalRequest.isValidToken(cleanToken) || action == null) {
            throw new IllegalArgumentException("invalid approval decision");
        }
        this.token = cleanToken;
        this.action = action;
    }

    public String token() {
        return token;
    }

    public RemoteIMApprovalAction action() {
        return action;
    }

    @Override
    public boolean equals(Object other) {
        if (this == other) return true;
        if (!(other instanceof RemoteIMApprovalDecision)) return false;
        RemoteIMApprovalDecision that = (RemoteIMApprovalDecision) other;
        return token.equals(that.token) && action == that.action;
    }

    @Override
    public int hashCode() {
        return Objects.hash(token, action);
    }
}
