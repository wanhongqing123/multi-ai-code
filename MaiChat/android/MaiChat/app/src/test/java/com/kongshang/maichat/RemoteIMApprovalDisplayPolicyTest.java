package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;

import org.junit.Test;

import java.util.List;
import java.util.Map;

public class RemoteIMApprovalDisplayPolicyTest {
    @Test
    public void pendingSentAndFailedDecisionsDriveRequestState() {
        ChatState state = new ChatState("android-user");
        state.upsertContact(new RemoteIMContact("desktop-bot", "Desktop Bot"));
        state.selectPeer("desktop-bot");
        RemoteIMApprovalRequest request = request("approval-display-1");
        state.receiveText(
            "需要审批",
            "desktop-bot",
            "remote-request-1",
            1_700_000_000_000L,
            RemoteIMOrigin.MACHINE,
            request
        );

        assertEquals(
            RemoteIMApprovalDisplayPolicy.State.AVAILABLE,
            displayState(state, request)
        );

        RemoteIMMessage first = state.queueOutgoingApprovalDecision(
            "desktop-bot",
            request.token(),
            RemoteIMApprovalAction.APPROVE_ONCE
        );
        assertEquals(
            RemoteIMApprovalDisplayPolicy.State.SENDING,
            displayState(state, request)
        );

        state.updateMessageStatus(first.id(), RemoteIMMessage.Status.FAILED);
        assertEquals(
            RemoteIMApprovalDisplayPolicy.State.AVAILABLE,
            displayState(state, request)
        );

        RemoteIMMessage retry = state.queueOutgoingApprovalDecision(
            "desktop-bot",
            request.token(),
            RemoteIMApprovalAction.REJECT
        );
        state.updateMessageStatus(retry.id(), RemoteIMMessage.Status.SENT);
        assertEquals(
            RemoteIMApprovalDisplayPolicy.State.SENT,
            displayState(state, request)
        );
    }

    @Test
    public void sentDecisionWinsOverLaterPendingDuplicate() {
        ChatState state = new ChatState("android-user");
        RemoteIMApprovalRequest request = request("approval-display-2");
        RemoteIMMessage sent = state.queueOutgoingApprovalDecision(
            "desktop-bot",
            request.token(),
            RemoteIMApprovalAction.APPROVE_ONCE
        );
        state.updateMessageStatus(sent.id(), RemoteIMMessage.Status.SENT);
        state.queueOutgoingApprovalDecision(
            "desktop-bot",
            request.token(),
            RemoteIMApprovalAction.REJECT
        );

        assertEquals(
            RemoteIMApprovalDisplayPolicy.State.SENT,
            displayState(state, request)
        );
    }

    private static RemoteIMApprovalDisplayPolicy.State displayState(
        ChatState state,
        RemoteIMApprovalRequest request
    ) {
        Map<String, RemoteIMApprovalDisplayPolicy.State> states =
            RemoteIMApprovalDisplayPolicy.statesFor(state.messagesWith("desktop-bot"));
        return RemoteIMApprovalDisplayPolicy.stateFor(request, states);
    }

    private static RemoteIMApprovalRequest request(String token) {
        return new RemoteIMApprovalRequest(
            token,
            List.of(RemoteIMApprovalAction.APPROVE_ONCE, RemoteIMApprovalAction.REJECT)
        );
    }
}
