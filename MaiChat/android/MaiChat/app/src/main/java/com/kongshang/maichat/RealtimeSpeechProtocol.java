package com.kongshang.maichat;

import org.json.JSONObject;
import java.net.URLEncoder;
import java.nio.charset.StandardCharsets;
import java.util.Base64;
import java.util.Map;
import java.util.TreeMap;
import javax.crypto.Mac;
import javax.crypto.spec.SecretKeySpec;

/** Tencent realtime ASR v1 wire contract; no microphone, socket or UI dependencies. */
final class RealtimeSpeechProtocol {
    private final TreeMap<Integer, String> sentences = new TreeMap<>();
    private final java.util.Set<Integer> committed = new java.util.HashSet<>();
    private boolean complete;
    void accept(JSONObject message) {
        JSONObject result = message.optJSONObject("result");
        if (result != null) {
            int index = result.optInt("index", -1);
            int type = result.optInt("slice_type", -1);
            if (index >= 0 && index < 10000 && type >= 0 && type <= 2 && !committed.contains(index)) {
                sentences.put(index, result.optString("voice_text_str", ""));
                if (type == 2) committed.add(index);
            }
        }
        if (message.optInt("final", 0) == 1) complete = true;
    }
    boolean complete() { return complete; }
    String text() {
        StringBuilder text = new StringBuilder();
        for (String sentence : sentences.values()) text.append(sentence);
        return text.toString().trim();
    }
    static String signedUrl(String appId, String secretId, String secretKey, String voiceId, long timestamp, int nonce) throws Exception {
        if (!appId.matches("[0-9]{5,20}")) throw new IllegalArgumentException("Invalid speech account");
        TreeMap<String, String> parameters = new TreeMap<>();
        parameters.put("engine_model_type", "16k_zh"); parameters.put("expired", Long.toString(timestamp + 3600));
        parameters.put("needvad", "1"); parameters.put("nonce", Integer.toString(nonce));
        parameters.put("secretid", secretId); parameters.put("timestamp", Long.toString(timestamp));
        parameters.put("voice_format", "16"); parameters.put("voice_id", voiceId);
        StringBuilder query = new StringBuilder();
        for (Map.Entry<String, String> entry : parameters.entrySet()) {
            if (query.length() > 0) query.append('&');
            query.append(entry.getKey()).append('=').append(entry.getValue());
        }
        String endpoint = "asr.cloud.tencent.com/asr/v2/" + appId;
        Mac hmac = Mac.getInstance("HmacSHA1");
        hmac.init(new SecretKeySpec(secretKey.getBytes(StandardCharsets.UTF_8), "HmacSHA1"));
        String signature = Base64.getEncoder().encodeToString(hmac.doFinal((endpoint + "?" + query).getBytes(StandardCharsets.UTF_8)));
        return "wss://" + endpoint + "?" + query + "&signature=" + URLEncoder.encode(signature, "UTF-8");
    }
}
