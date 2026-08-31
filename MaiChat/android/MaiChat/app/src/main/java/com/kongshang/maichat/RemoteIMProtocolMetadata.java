package com.kongshang.maichat;

import com.google.gson.Gson;

import java.util.ArrayList;
import java.util.List;
import java.util.Map;

public final class RemoteIMProtocolMetadata {
    public static final int VERSION = 2;
    private static final String NAMESPACE = "multi-ai-code";
    private static final Gson GSON = new Gson();

    public static final class Metadata {
        private final RemoteIMOrigin origin;
        private final RemoteIMApprovalRequest approvalRequest;
        private final RemoteIMApprovalDecision approvalDecision;
        private final boolean captionAbove;
        private final RemoteIMQuote quote;

        Metadata(
            RemoteIMOrigin origin,
            RemoteIMApprovalRequest approvalRequest,
            RemoteIMApprovalDecision approvalDecision,
            boolean captionAbove,
            RemoteIMQuote quote
        ) {
            this.origin = origin == null ? RemoteIMOrigin.MACHINE : origin;
            this.approvalRequest = approvalRequest;
            this.approvalDecision = approvalDecision;
            this.captionAbove = captionAbove;
            this.quote = quote;
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

        public boolean captionAbove() {
            return captionAbove;
        }

        public RemoteIMQuote quote() {
            return quote;
        }
    }

    private static final class WireMetadata {
        String namespace;
        int version;
        String origin;
        WireInteraction interaction;
        Object captionAbove;
        Object quote;
    }

    private static final class WireQuote {
        Object msgId;
        Object sender;
        Object digest;
        Object kind;
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
        return encode(origin, null, null, null);
    }

    public static String encode(RemoteIMOrigin origin, RemoteIMQuote quote) {
        return encode(origin, null, null, quote);
    }

    public static String encodeApprovalRequest(RemoteIMApprovalRequest request) {
        if (request == null) throw new IllegalArgumentException("approval request is required");
        return encode(RemoteIMOrigin.MACHINE, request, null, null);
    }

    public static String encodeApprovalDecision(RemoteIMApprovalDecision decision) {
        if (decision == null) throw new IllegalArgumentException("approval decision is required");
        if (decision.action() != RemoteIMApprovalAction.APPROVE_ONCE
            && decision.action() != RemoteIMApprovalAction.APPROVE_PREFIX
            && decision.action() != RemoteIMApprovalAction.REJECT) {
            throw new IllegalArgumentException("approval resolution cannot be sent as a user decision");
        }
        return encode(RemoteIMOrigin.HUMAN, null, decision, null);
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
            || wire.version < VERSION) {
            return machineMetadata();
        }
        if (!"human".equals(wire.origin) && !"machine".equals(wire.origin)) {
            return machineMetadata();
        }
        RemoteIMOrigin origin = RemoteIMOrigin.fromWireValue(wire.origin);
        boolean captionAbove = Boolean.TRUE.equals(wire.captionAbove);
        RemoteIMQuote quote = decodeQuote(wire.quote);
        if (wire.interaction == null) return new Metadata(origin, null, null, captionAbove, quote);

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
                    null,
                    captionAbove,
                    quote
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
                    new RemoteIMApprovalDecision(interaction.token, action),
                    captionAbove,
                    quote
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
                    new RemoteIMApprovalDecision(interaction.token, action),
                    captionAbove,
                    quote
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
        RemoteIMApprovalDecision decision,
        RemoteIMQuote quote
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
        if (quote != null && !quote.digest().isEmpty()) {
            WireQuote wireQuote = new WireQuote();
            if (!quote.messageId().isEmpty()) wireQuote.msgId = quote.messageId();
            wireQuote.sender = quote.senderId();
            wireQuote.digest = quote.digest();
            wireQuote.kind = quote.kind();
            wire.quote = wireQuote;
        }
        return GSON.toJson(wire);
    }

    private static RemoteIMQuote decodeQuote(Object value) {
        if (!(value instanceof Map)) return null;
        Map<?, ?> wire = (Map<?, ?>) value;
        Object rawDigest = wire.get("digest");
        if (!(rawDigest instanceof String)) return null;
        String digest = clamp((String) rawDigest, 200);
        if (digest.isEmpty()) return null;
        Object rawMessageId = wire.get("msgId");
        Object rawSender = wire.get("sender");
        Object rawKind = wire.get("kind");
        String messageId = rawMessageId instanceof String ? clamp((String) rawMessageId, 256) : "";
        String sender = rawSender instanceof String ? clamp((String) rawSender, 256) : "";
        String kind = rawKind instanceof String ? clamp((String) rawKind, 256) : "";
        try {
            return new RemoteIMQuote(messageId, sender, digest, kind);
        } catch (IllegalArgumentException error) {
            return null;
        }
    }

    private static String clamp(String value, int limit) {
        String clean = value == null ? "" : value.trim();
        return clean.length() <= limit ? clean : clean.substring(0, limit);
    }

    private static Metadata machineMetadata() {
        return new Metadata(RemoteIMOrigin.MACHINE, null, null, false, null);
    }
}
