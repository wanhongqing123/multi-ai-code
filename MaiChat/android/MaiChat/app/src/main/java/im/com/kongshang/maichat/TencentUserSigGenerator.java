package com.kongshang.maichat;

import java.io.ByteArrayOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.Base64;
import java.util.zip.Deflater;

import javax.crypto.Mac;
import javax.crypto.spec.SecretKeySpec;

public final class TencentUserSigGenerator {
    private TencentUserSigGenerator() {
    }

    public static String generate(int sdkAppId, String userId, String secretKey) {
        return generate(sdkAppId, userId, secretKey, 604_800, System.currentTimeMillis() / 1000L);
    }

    static String generate(
        int sdkAppId,
        String userId,
        String secretKey,
        int expireSeconds,
        long currentTimeSeconds
    ) {
        String cleanUserId = clean(userId);
        String cleanSecret = clean(secretKey);
        if (sdkAppId <= 0) throw new IllegalArgumentException("IM 应用配置无效");
        if (cleanUserId.isEmpty()) throw new IllegalArgumentException("请填写账号 ID");
        if (cleanSecret.isEmpty()) throw new IllegalArgumentException("内置连接凭证无效");

        String content = "TLS.identifier:" + cleanUserId + "\n"
            + "TLS.sdkappid:" + sdkAppId + "\n"
            + "TLS.time:" + currentTimeSeconds + "\n"
            + "TLS.expire:" + expireSeconds + "\n";
        String signature = hmacSha256Base64(cleanSecret, content);
        String json = "{"
            + "\"TLS.ver\":\"2.0\","
            + "\"TLS.identifier\":\"" + escapeJson(cleanUserId) + "\","
            + "\"TLS.sdkappid\":" + sdkAppId + ","
            + "\"TLS.expire\":" + expireSeconds + ","
            + "\"TLS.time\":" + currentTimeSeconds + ","
            + "\"TLS.sig\":\"" + escapeJson(signature) + "\""
            + "}";
        return Base64.getEncoder()
            .encodeToString(deflate(json.getBytes(StandardCharsets.UTF_8)))
            .replace('+', '*')
            .replace('/', '-')
            .replace('=', '_');
    }

    private static String hmacSha256Base64(String secretKey, String content) {
        try {
            Mac mac = Mac.getInstance("HmacSHA256");
            mac.init(new SecretKeySpec(secretKey.getBytes(StandardCharsets.UTF_8), "HmacSHA256"));
            return Base64.getEncoder().encodeToString(
                mac.doFinal(content.getBytes(StandardCharsets.UTF_8))
            );
        } catch (Exception error) {
            throw new IllegalStateException("生成登录凭证失败", error);
        }
    }

    private static byte[] deflate(byte[] input) {
        Deflater deflater = new Deflater(Deflater.DEFAULT_COMPRESSION, false);
        deflater.setInput(input);
        deflater.finish();
        ByteArrayOutputStream output = new ByteArrayOutputStream(input.length + 64);
        byte[] buffer = new byte[512];
        while (!deflater.finished()) {
            int count = deflater.deflate(buffer);
            if (count <= 0 && deflater.needsInput()) break;
            output.write(buffer, 0, count);
        }
        deflater.end();
        return output.toByteArray();
    }

    private static String escapeJson(String value) {
        StringBuilder result = new StringBuilder(value.length() + 8);
        for (int index = 0; index < value.length(); index += 1) {
            char character = value.charAt(index);
            switch (character) {
                case '\\': result.append("\\\\"); break;
                case '"': result.append("\\\""); break;
                case '\n': result.append("\\n"); break;
                case '\r': result.append("\\r"); break;
                case '\t': result.append("\\t"); break;
                default:
                    if (character < 0x20) {
                        result.append(String.format("\\u%04x", (int) character));
                    } else {
                        result.append(character);
                    }
            }
        }
        return result.toString();
    }

    private static String clean(String value) {
        return value == null ? "" : value.trim();
    }
}
