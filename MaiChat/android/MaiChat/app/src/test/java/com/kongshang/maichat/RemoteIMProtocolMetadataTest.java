package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

import java.util.List;

public class RemoteIMProtocolMetadataTest {
    @Test
    public void encodesVersionTwoHumanMessagesFromMaiChatUi() {
        String value = RemoteIMProtocolMetadata.encode(RemoteIMOrigin.HUMAN);
        RemoteIMProtocolMetadata.Metadata metadata =
            RemoteIMProtocolMetadata.decodeMetadata(value);

        assertEquals(
            "{\"namespace\":\"multi-ai-code\",\"version\":2,\"origin\":\"human\"}",
            value
        );
        assertEquals(RemoteIMOrigin.HUMAN, metadata.origin());
        assertNull(metadata.approvalRequest());
        assertNull(metadata.approvalDecision());
        assertFalse(metadata.captionAbove());
    }

    @Test
    public void captionPlacementIsAnOptionalAdditiveVersionTwoKey() {
        RemoteIMProtocolMetadata.Metadata above = RemoteIMProtocolMetadata.decodeMetadata(
            "{\"namespace\":\"multi-ai-code\",\"version\":2,\"origin\":\"human\","
                + "\"captionAbove\":true}"
        );
        RemoteIMProtocolMetadata.Metadata missing = RemoteIMProtocolMetadata.decodeMetadata(
            "{\"namespace\":\"multi-ai-code\",\"version\":2,\"origin\":\"human\"}"
        );
        RemoteIMProtocolMetadata.Metadata wrongType = RemoteIMProtocolMetadata.decodeMetadata(
            "{\"namespace\":\"multi-ai-code\",\"version\":2,\"origin\":\"human\","
                + "\"captionAbove\":\"true\"}"
        );

        assertTrue(above.captionAbove());
        assertFalse(missing.captionAbove());
        assertFalse(wrongType.captionAbove());
        assertEquals(2, RemoteIMProtocolMetadata.VERSION);
        assertFalse(RemoteIMProtocolMetadata.encode(RemoteIMOrigin.HUMAN).contains("captionAbove"));
    }

    @Test
    public void roundTripsApprovalRequests() {
        RemoteIMApprovalRequest request = new RemoteIMApprovalRequest(
            "approval-wire-android-1",
            List.of(
                RemoteIMApprovalAction.APPROVE_ONCE,
                RemoteIMApprovalAction.APPROVE_PREFIX,
                RemoteIMApprovalAction.REJECT
            )
        );

        String encoded = RemoteIMProtocolMetadata.encodeApprovalRequest(request);
        RemoteIMProtocolMetadata.Metadata metadata =
            RemoteIMProtocolMetadata.decodeMetadata(encoded);

        assertEquals(
            "{\"namespace\":\"multi-ai-code\",\"version\":2,\"origin\":\"machine\","
                + "\"interaction\":{\"kind\":\"approval-request\","
                + "\"token\":\"approval-wire-android-1\","
                + "\"actions\":[\"approve-once\",\"approve-prefix\",\"reject\"]}}",
            encoded
        );
        assertEquals(RemoteIMOrigin.MACHINE, metadata.origin());
        assertEquals(request, metadata.approvalRequest());
        assertNull(metadata.approvalDecision());
    }

    @Test
    public void roundTripsApprovalDecisions() {
        RemoteIMApprovalDecision decision = new RemoteIMApprovalDecision(
            "approval-wire-android-2",
            RemoteIMApprovalAction.APPROVE_ONCE
        );

        String encoded = RemoteIMProtocolMetadata.encodeApprovalDecision(decision);
        RemoteIMProtocolMetadata.Metadata metadata =
            RemoteIMProtocolMetadata.decodeMetadata(encoded);

        assertEquals(
            "{\"namespace\":\"multi-ai-code\",\"version\":2,\"origin\":\"human\","
                + "\"interaction\":{\"kind\":\"approval-decision\","
                + "\"token\":\"approval-wire-android-2\",\"action\":\"approve-once\"}}",
            encoded
        );
        assertEquals(RemoteIMOrigin.HUMAN, metadata.origin());
        assertNull(metadata.approvalRequest());
        assertEquals(decision, metadata.approvalDecision());
    }

    @Test
    public void decodesMachineApprovalResolutionAsAuthoritativeDecisionState() {
        RemoteIMProtocolMetadata.Metadata metadata = RemoteIMProtocolMetadata.decodeMetadata(
            "{\"namespace\":\"multi-ai-code\",\"version\":2,\"origin\":\"machine\","
                + "\"interaction\":{\"kind\":\"approval-resolved\","
                + "\"token\":\"approval-wire-android-3\",\"outcome\":\"auto-declined\"}}"
        );
        assertEquals(RemoteIMOrigin.MACHINE, metadata.origin());
        assertNull(metadata.approvalRequest());
        assertEquals(
            new RemoteIMApprovalDecision(
                "approval-wire-android-3",
                RemoteIMApprovalAction.AUTO_DECLINED
            ),
            metadata.approvalDecision()
        );
    }

    @Test
    public void versionOneAndMalformedMetadataFailClosedWithoutInteraction() {
        for (String value : new String[]{
            null,
            "{\"namespace\":\"multi-ai-code\",\"version\":1,\"origin\":\"human\"}",
            "{\"namespace\":\"multi-ai-code\",\"version\":2,\"origin\":\"invalid\"}",
            "{\"namespace\":\"foreign\",\"version\":2,\"origin\":\"human\"}"
        }) {
            RemoteIMProtocolMetadata.Metadata metadata =
                RemoteIMProtocolMetadata.decodeMetadata(value);
            assertEquals(RemoteIMOrigin.MACHINE, metadata.origin());
            assertNull(metadata.approvalRequest());
            assertNull(metadata.approvalDecision());
        }
    }

    @Test
    public void rejectsWrongDirectionAndInvalidApprovalCapabilities() {
        String humanRequest = "{\"namespace\":\"multi-ai-code\",\"version\":2,"
            + "\"origin\":\"human\",\"interaction\":{\"kind\":\"approval-request\","
            + "\"token\":\"approval-bad-1\",\"actions\":[\"approve-once\",\"reject\"]}}";
        String duplicateActions = "{\"namespace\":\"multi-ai-code\",\"version\":2,"
            + "\"origin\":\"machine\",\"interaction\":{\"kind\":\"approval-request\","
            + "\"token\":\"approval-bad-2\",\"actions\":[\"approve-once\",\"approve-once\",\"reject\"]}}";

        for (String value : new String[]{humanRequest, duplicateActions}) {
            RemoteIMProtocolMetadata.Metadata metadata =
                RemoteIMProtocolMetadata.decodeMetadata(value);
            assertEquals(RemoteIMOrigin.MACHINE, metadata.origin());
            assertNull(metadata.approvalRequest());
            assertNull(metadata.approvalDecision());
        }
    }
}
