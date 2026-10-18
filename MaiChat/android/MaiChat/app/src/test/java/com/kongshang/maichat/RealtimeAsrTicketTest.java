// 文件作用：验证账号服务签发的实时语音地址只会连接预期供应商和本次 voice_id。
package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.fail;

import java.io.IOException;
import org.json.JSONObject;
import org.junit.Test;

public final class RealtimeAsrTicketTest {
    private static final long NOW = 1_780_000_000L;
    private static final String VOICE = "c64385ee-3e5c-4fc5-bbfd-7c71addb35b0";

    private JSONObject response(String url) throws Exception {
        return new JSONObject().put("voice_id", VOICE).put("url", url)
            .put("expires_at", NOW + 300);
    }

    private String url(String host, String voice) {
        return "wss://" + host + "/asr/v2/1259220000?engine_model_type=16k_zh"
            + "&expired=" + (NOW + 300) + "&needvad=1&nonce=7&secretid=test-id"
            + "&timestamp=" + NOW + "&voice_format=16&voice_id=" + voice
            + "&signature=test%2Bsignature%3D";
    }

    @Test public void acceptsOnlyTheSignedTencentEndpointForThisRecording() throws Exception {
        MaiChatAuthClient.RealtimeAsrTicket ticket = MaiChatAuthClient.parseRealtimeAsrTicket(
            response(url("asr.cloud.tencent.com", VOICE)), NOW);
        assertEquals(VOICE, ticket.voiceId);
        assertEquals(NOW + 300, ticket.expiresAt);
        for (String rejected : new String[] {
            url("attacker.example", VOICE),
            url("asr.cloud.tencent.com", "00000000-0000-0000-0000-000000000000"),
            url("asr.cloud.tencent.com", VOICE).replace("voice_format=16", "voice_format=1")
        }) {
            try {
                MaiChatAuthClient.parseRealtimeAsrTicket(response(rejected), NOW);
                fail("Unexpected realtime ASR endpoint accepted");
            } catch (IOException expected) { }
        }
    }
}
