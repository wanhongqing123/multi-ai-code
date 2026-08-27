package com.kongshang.maichat;

import com.google.gson.Gson;

import java.util.ArrayList;
import java.util.List;

public final class RemoteIMProtocolMetadata {
    public static final int VERSION = 2;
    private static final String NAMESPACE = "multi-ai-code";
    private static final Gson GSON = new Gson();

    public static final class Metadata {
        private final RemoteIMOrigin origin;
        private final RemoteIMApprovalRequest approvalRequest;
        private final RemoteIMApprovalDecision approvalDecision;

        Metadata(
            RemoteIMOrigin origin,
            RemoteIMApprovalRequest approvalRequest,
            RemoteIMApprovalDecision approvalDecision
        ) {
            this.origin = origin == null ? RemoteIMOrigin.MACHINE : origin;
            this.approvalRequest = approvalRequest;
            this.approvalDecision = approvalDecision;
        }

        public RemoteIMOrigin origin() {
            return origin;
        }

        public RemoteIMApprovalRequest approvalRequest() {
            return approvalRequest;
        }

        public RemoteIMApprovalDecision approvalDecision() {
            return approvalDecision;
        }
    }

    private static final class WireMetadata {
        String namespace;
        int version;
        String origin;
        WireInteraction interaction;
    }

    private static final class WireInteraction {
        String kind;
        String token;
        List<String> actions;
        String action;
        String outcome;
    }

    private RemoteIMProtocolMetadata() {
    }

    public static String encode(RemoteIMOrigin origin) {
        return encode(origin, null, null);
    }

    public static String encodeApprovalRequest(RemoteIMApprovalRequest request) {
        if (request == null) throw new IllegalArgumentException("approval request is required");
        return encode(RemoteIMOrigin.MACHINE, request, null);
    }

    public static String encodeApprovalDecision(RemoteIMApprovalDecision decision) {
        if (decision == null) throw new IllegalArgumentException("approval decision is required");
        if (decision.action() != RemoteIMApprovalAction.APPROVE_ONCE
            && decision.action() != RemoteIMApprovalAction.APPROVE_PREFIX
            && decision.action() != RemoteIMApprovalAction.REJECT) {
            throw new IllegalArgumentException("approval resolution cannot be sent as a user decision");
        }
        return encode(RemoteIMOrigin.HUMAN, null, decision);
    }

    public static RemoteIMOrigin decode(String value) {
        return decodeMetadata(value).origin();
    }

    public static Metadata decodeMetadata(String value) {
        if (value == null || value.trim().isEmpty()) return machineMetadata();
        final WireMetadata wire;
        try {
            wire = GSON.fromJson(value, WireMetadata.class);
        } catch (RuntimeException error) {
            return machineMetadata();
        }
        if (wire == null
            || !NAMESPACE.equals(wire.namespace)
            || wire.version != VERSION) {
            return machineMetadata();
        }
        if (!"human".equals(wire.origin) && !"machine".equals(wire.origin)) {
            return machineMetadata();
        }
        RemoteIMOrigin origin = RemoteIMOrigin.fromWireValue(wire.origin);
        if (wire.interaction == null) return new Metadata(origin, null, null);

        WireInteraction interaction = wire.interaction;
        if ("approval-request".equals(interaction.kind)
            && origin == RemoteIMOrigin.MACHINE
            && interaction.action == null
            && interaction.actions != null) {
            List<RemoteIMApprovalAction> actions = new ArrayList<>();
            for (String rawAction : interaction.actions) {
                RemoteIMApprovalAction action = RemoteIMApprovalAction.fromWireValue(rawAction);
                if (action == null) return machineMetadata();
                actions.add(action);
            }
            try {
                return new Metadata(
                    origin,
                    new RemoteIMApprovalRequest(interaction.token, actions),
                    null
                );
            } catch (IllegalArgumentException error) {
                return machineMetadata();
            }
        }

        if ("approval-decision".equals(interaction.kind)
            && origin == RemoteIMOrigin.HUMAN
            && interaction.actions == null) {
            RemoteIMApprovalAction action = RemoteIMApprovalAction.fromWireValue(interaction.action);
            if (action != RemoteIMApprovalAction.APPROVE_ONCE
                && action != RemoteIMApprovalAction.APPROVE_PREFIX
                && action != RemoteIMApprovalAction.REJECT) return machineMetadata();
            try {
                return new Metadata(
                    origin,
                    null,
                    new RemoteIMApprovalDecision(interaction.token, action)
                );
            } catch (IllegalArgumentException error) {
                return machineMetadata();
            }
        }
        if ("approval-resolved".equals(interaction.kind)
            && origin == RemoteIMOrigin.MACHINE
            && interaction.actions == null
            && interaction.action == null) {
            RemoteIMApprovalAction action = "auto-declined".equals(interaction.outcome)
                ? RemoteIMApprovalAction.AUTO_DECLINED
                : ("approved".equals(interaction.outcome)
                    || "rejected".equals(interaction.outcome)
                    || "resolved".equals(interaction.outcome))
                        ? RemoteIMApprovalAction.RESOLVED
                        : null;
            if (action == null) return machineMetadata();
            try {
                return new Metadata(
                    origin,
                    null,
                    new RemoteIMApprovalDecision(interaction.token, action)
                );
            } catch (IllegalArgumentException error) {
                return machineMetadata();
            }
        }
        return machineMetadata();
    }

    private static String encode(
        RemoteIMOrigin origin,
        RemoteIMApprovalRequest request,
        RemoteIMApprovalDecision decision
    ) {
        WireMetadata wire = new WireMetadata();
        wire.namespace = NAMESPACE;
        wire.version = VERSION;
        wire.origin = (origin == null ? RemoteIMOrigin.MACHINE : origin).wireValue();
        if (request != null) {
            WireInteraction interaction = new WireInteraction();
            interaction.kind = "approval-request";
            interaction.token = request.token();
            interaction.actions = new ArrayList<>();
            for (RemoteIMApprovalAction action : request.actions()) {
                interaction.actions.add(action.wireValue());
            }
            wire.interaction = interaction;
        } else if (decision != null) {
            WireInteraction interaction = new WireInteraction();
            interaction.kind = "approval-decision";
            interaction.token = decision.token();
            interaction.action = decision.action().wireValue();
            wire.interaction = interaction;
        }
        return GSON.toJson(wire);
    }

    private static Metadata machineMetadata() {
        return new Metadata(RemoteIMOrigin.MACHINE, null, null);
    }
}
