// 文件作用：校验内存中的 ASR STS 临时凭据，并按腾讯 V2 协议为每次录音独立签名。
// SecretKey 和 token 不落盘、不进 URL/日志；token 由传输层放入固定 X-TC-Token 头。
package com.kongshang.maichat;

import java.io.IOException;
import java.net.URLEncoder;
import java.nio.charset.StandardCharsets;
import java.security.GeneralSecurityException;
import java.security.SecureRandom;
import java.util.Base64;
import java.util.Map;
import java.util.TreeMap;
import java.util.UUID;
import javax.crypto.Mac;
import javax.crypto.spec.SecretKeySpec;
import org.json.JSONObject;

final class RealtimeAsrCredential {
    private static final SecureRandom random = new SecureRandom();
    private final String appId, secretId, secretKey, token;
    final long expiresAt;

    private RealtimeAsrCredential(String appId, String secretId, String secretKey,
                                  String token, long expiresAt) {
        this.appId = appId; this.secretId = secretId; this.secretKey = secretKey;
        this.token = token; this.expiresAt = expiresAt;
    }

    static RealtimeAsrCredential parse(JSONObject issued, long now) throws IOException {
        String appId = issued.optString("app_id", "");
        String secretId = issued.optString("secret_id", "");
        String secretKey = issued.optString("secret_key", "");
        String token = issued.optString("token", "");
        long expiresAt = issued.optLong("expires_at", 0);
        if (!(issued.opt("app_id") instanceof String) || !(issued.opt("secret_id") instanceof String) ||
            !(issued.opt("secret_key") instanceof String) || !(issued.opt("token") instanceof String) ||
            !(issued.opt("expires_at") instanceof Number) ||
            ((Number) issued.opt("expires_at")).doubleValue() != expiresAt ||
            !appId.matches("[0-9]{5,20}") || !secretId.matches("[A-Za-z0-9]{8,128}") ||
            !secretKey.matches("[!-~]{16,256}") || !token.matches("[!-~]{16,8192}") ||
            expiresAt <= now + 90 || expiresAt > now + 1860)
            throw new IOException("语音识别临时凭据无效");
        return new RealtimeAsrCredential(appId, secretId, secretKey, token, expiresAt);
    }

    MaiChatAuthClient.RealtimeAsrTicket newTicket(long now) throws IOException {
        return sign(now, UUID.randomUUID().toString(), 1L + random.nextInt(Integer.MAX_VALUE));
    }

    // 131127/48982 与官方 Java SDK：先按未编码参数排序做 HmacSHA1，之后编码签名。
    // 官方 core/ws/WebSocketClient 通过 X-TC-Token 发送 STS token，不把它混入 canonical。
    MaiChatAuthClient.RealtimeAsrTicket sign(long now, String voiceId, long nonce) throws IOException {
        if (expiresAt <= now + 90 || !voiceId.matches(
                "[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}") || nonce <= 0 || nonce > 9999999999L)
            throw new IOException("语音识别临时凭据或录音标识无效");
        long expired = Math.min(now + 300, expiresAt);
        TreeMap<String, String> parameters = new TreeMap<>();
        parameters.put("engine_model_type", "16k_zh_en_2.0");
        parameters.put("expired", Long.toString(expired));
        parameters.put("needvad", "1");
        parameters.put("nonce", Long.toString(nonce));
        // 官方 V2 SDK 固定句子结果模式；不启用说话人分离等附加能力。
        parameters.put("result_mod", "1");
        parameters.put("secretid", secretId);
        parameters.put("timestamp", Long.toString(now));
        parameters.put("voice_format", "16");
        parameters.put("voice_id", voiceId);
        StringBuilder canonical = new StringBuilder("asr.cloud.tencent.com/asr/v2/")
            .append(appId).append('?');
        for (Map.Entry<String, String> entry : parameters.entrySet()) {
            if (canonical.charAt(canonical.length() - 1) != '?') canonical.append('&');
            canonical.append(entry.getKey()).append('=').append(entry.getValue());
        }
        try {
            Mac hmac = Mac.getInstance("HmacSHA1");
            hmac.init(new SecretKeySpec(secretKey.getBytes(StandardCharsets.UTF_8), "HmacSHA1"));
            String signature = Base64.getEncoder().encodeToString(
                hmac.doFinal(canonical.toString().getBytes(StandardCharsets.UTF_8)));
            String url = "wss://" + canonical + "&signature=" + URLEncoder.encode(signature, "UTF-8");
            return new MaiChatAuthClient.RealtimeAsrTicket(voiceId, url, token, expired);
        } catch (GeneralSecurityException error) {
            throw new IOException("语音识别签名生成失败");
        }
    }
}
