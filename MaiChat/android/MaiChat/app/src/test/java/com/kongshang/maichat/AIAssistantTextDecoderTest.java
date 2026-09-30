package com.kongshang.maichat;

import org.junit.Test;

import java.nio.charset.CharacterCodingException;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.fail;

public class AIAssistantTextDecoderTest {
    @Test public void convertsGb18030AndRejectsWrongEncoding() throws Exception {
        byte[] legacy = {(byte) 0xC4, (byte) 0xE3, (byte) 0xBA, (byte) 0xC3};
        assertEquals("你好", AIAssistantTextDecoder.decode(legacy, "auto"));
        assertEquals("你好", AIAssistantTextDecoder.decode(legacy, "GB18030"));
        try {
            AIAssistantTextDecoder.decode(legacy, "UTF-8");
            fail("Invalid UTF-8 must be rejected, not silently replaced");
        } catch (CharacterCodingException expected) {
            // The caller can retry with the correct source encoding.
        }
    }
}
