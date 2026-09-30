package com.kongshang.maichat;

import java.nio.ByteBuffer;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.Charset;
import java.nio.charset.CodingErrorAction;

/** Converts legacy file text to Unicode before the Agent returns it as UTF-8 JSON. */
final class AIAssistantTextDecoder {
    private AIAssistantTextDecoder() {}

    static String decode(byte[] bytes, String requested) throws CharacterCodingException {
        String normalized = requested == null ? "auto" : requested.trim().toLowerCase(java.util.Locale.ROOT);
        String name = normalized.equals("auto") || normalized.equals("system") ? "GB18030"
            : normalized.equals("gbk") || normalized.equals("cp936") ? "GBK" : requested;
        return Charset.forName(name).newDecoder()
            .onMalformedInput(CodingErrorAction.REPORT)
            .onUnmappableCharacter(CodingErrorAction.REPORT)
            .decode(ByteBuffer.wrap(bytes)).toString();
    }
}
