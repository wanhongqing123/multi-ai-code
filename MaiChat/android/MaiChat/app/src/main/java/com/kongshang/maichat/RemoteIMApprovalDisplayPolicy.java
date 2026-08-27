package com.kongshang.maichat;

import java.util.HashMap;
import java.util.List;
import java.util.Map;

public final class RemoteIMApprovalDisplayPolicy {
    public enum State {
        AVAILABLE,
        SENDING,
        SENT,
        RESOLVED,
        AUTO_DECLINED
    }

    private RemoteIMApprovalDisplayPolicy() {
    }

    public static Map<String, State> statesFor(List<RemoteIMMessage> messages) {
        Map<String, State> states = new HashMap<>();
        if (messages == null) return states;
        for (RemoteIMMessage message : messages) {
            if (message == null || message.approvalDecision() == null) {
                continue;
            }
            RemoteIMApprovalDecision decision = message.approvalDecision();
            String token = decision.token();
            if (decision.action() == RemoteIMApprovalAction.AUTO_DECLINED) {
                states.put(token, State.AUTO_DECLINED);
            } else if (decision.action() == RemoteIMApprovalAction.RESOLVED
                && states.get(token) != State.AUTO_DECLINED) {
                states.put(token, State.RESOLVED);
            } else if (states.get(token) == State.RESOLVED
                || states.get(token) == State.AUTO_DECLINED) {
                continue;
            } else if (message.direction() == RemoteIMMessage.Direction.OUTGOING
                && message.status() == RemoteIMMessage.Status.SENT) {
                states.put(token, State.SENT);
            } else if (message.direction() == RemoteIMMessage.Direction.OUTGOING
                && message.status() == RemoteIMMessage.Status.PENDING
                && states.get(token) != State.SENT) {
                states.put(token, State.SENDING);
            }
            // FAILED is deliberately ignored: the original request becomes actionable again.
        }
        return states;
    }

    public static State stateFor(
        RemoteIMApprovalRequest request,
        Map<String, State> states
    ) {
        if (request == null || states == null) return State.AVAILABLE;
        State state = states.get(request.token());
        return state == null ? State.AVAILABLE : state;
    }
}
