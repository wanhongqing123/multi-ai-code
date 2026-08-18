package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;

import org.junit.Test;

public class RemoteIMProtocolMetadataTest {
    @Test
    public void encodesHumanMessagesFromMaiChatUi() {
        String value = RemoteIMProtocolMetadata.encode(RemoteIMOrigin.HUMAN);

        assertEquals(RemoteIMOrigin.HUMAN, RemoteIMProtocolMetadata.decode(value));
    }

    @Test
    public void malformedOrMissingOriginFailsClosedAsMachine() {
        assertEquals(RemoteIMOrigin.MACHINE, RemoteIMProtocolMetadata.decode(null));
        assertEquals(
            RemoteIMOrigin.MACHINE,
            RemoteIMProtocolMetadata.decode("{\"namespace\":\"multi-ai-code\",\"version\":1}")
        );
    }
}
