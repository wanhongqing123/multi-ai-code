package com.kongshang.maichat;

import java.util.regex.Matcher;
import java.util.regex.Pattern;

public final class RemoteIMProtocolMetadata {
    private static final Pattern ORIGIN_PATTERN = Pattern.compile(
        "\\\"origin\\\"\\s*:\\s*\\\"(human|machine)\\\""
    );

    private RemoteIMProtocolMetadata() {
    }

    public static String encode(RemoteIMOrigin origin) {
        RemoteIMOrigin resolved = origin == null ? RemoteIMOrigin.MACHINE : origin;
        return "{\"namespace\":\"multi-ai-code\",\"version\":1,\"origin\":\""
            + resolved.wireValue()
            + "\"}";
    }

    public static RemoteIMOrigin decode(String value) {
        if (value == null || !value.contains("\"namespace\":\"multi-ai-code\"")) {
            return RemoteIMOrigin.MACHINE;
        }
        Matcher matcher = ORIGIN_PATTERN.matcher(value);
        return matcher.find()
            ? RemoteIMOrigin.fromWireValue(matcher.group(1))
            : RemoteIMOrigin.MACHINE;
    }
}
