package com.kongshang.maichat;

import java.util.HashMap;
import java.util.List;
import java.util.Map;

public final class RemoteIMApprovalDisplayPolicy {
    public enum State {
        AVAILABLE,
        SENDING,
        SENT
    }

    private RemoteIMApprovalDisplayPolicy() {
    }

    public static Map<String, State> statesFor(List<RemoteIMMessage> messages) {
        Map<String, State> states = new HashMap<>();
        if (messages == null) return states;
        for (RemoteIMMessage message : messages) {
            if (message == null
                || message.direction() != RemoteIMMessage.Direction.OUTGOING
                || message.approvalDecision() == null) {
                continue;
            }
            String token = message.approvalDecision().token();
            if (message.status() == RemoteIMMessage.Status.SENT) {
                states.put(token, State.SENT);
            } else if (message.status() == RemoteIMMessage.Status.PENDING
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
