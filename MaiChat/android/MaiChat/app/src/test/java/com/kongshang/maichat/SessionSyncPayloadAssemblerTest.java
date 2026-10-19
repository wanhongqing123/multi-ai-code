// 文件作用：验证 Android 跨设备历史分块的来源、完整性与乱序重组。
package com.kongshang.maichat;

import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.Base64;
import org.json.JSONObject;
import org.junit.Test;
import static org.junit.Assert.*;

public class SessionSyncPayloadAssemblerTest {
    private static String digest(byte[] bytes) throws Exception {
        byte[] hash = MessageDigest.getInstance("SHA-256").digest(bytes);
        StringBuilder result = new StringBuilder();
        for (byte value : hash) result.append(String.format("%02x", value & 0xff));
        return result.toString();
    }

    private static JSONObject envelope(byte[] bytes, String hash, int index,
                                       String slot, String transfer) throws Exception {
        int middle = bytes.length / 2;
        byte[] part = index == 0 ? java.util.Arrays.copyOfRange(bytes, 0, middle)
            : java.util.Arrays.copyOfRange(bytes, middle, bytes.length);
        return new JSONObject().put("kind", "history_page").put("session_id", "ses_remote")
            .put("from_slot", slot).put("from_login_session_id", "owner-login")
            .put("payload", new JSONObject().put("transfer_id", transfer)
                .put("chunk_index", index).put("chunk_count", 2)
                .put("sha256", hash).put("data_base64", Base64.getEncoder().encodeToString(part)));
    }

    @Test public void outOfOrderChunksNeedTheSameOwnerAndDigest() throws Exception {
        byte[] body = "{\"ok\":true,\"messages\":[{\"id\":\"msg_1\"}]}"
            .getBytes(StandardCharsets.UTF_8);
        String hash = digest(body);
        SessionSyncPayloadAssembler assembler = new SessionSyncPayloadAssembler();
        assertNull(assembler.accept(envelope(body, hash, 1, "mac", "transfer-one")));
        assertNull(assembler.accept(envelope(body, hash, 0, "windows", "transfer-one")));
        JSONObject complete = assembler.accept(envelope(body, hash, 0, "mac", "transfer-one"));
        assertNotNull(complete);
        assertEquals("msg_1", complete.getJSONArray("messages").getJSONObject(0).getString("id"));

        assertNull(assembler.accept(envelope(body, "0".repeat(64), 0, "mac", "tampered")));
        assertNull(assembler.accept(envelope(body, "0".repeat(64), 1, "mac", "tampered")));
    }

    @Test public void commandFingerprintIsStableAndSeparatesDifferentTexts() throws Exception {
        JSONObject remote = new JSONObject().put("device_slot", "mac")
            .put("login_session_id", "owner-login").put("id", "ses_remote");
        JSONObject first = new JSONObject().put("action", "prompt")
            .put("text", "第一行\n第二行");
        JSONObject reordered = new JSONObject().put("text", "第一行\n第二行")
            .put("action", "prompt");
        JSONObject different = new JSONObject().put("action", "prompt")
            .put("text", "第一行\n第三行");
        assertEquals(AndroidSessionSyncCoordinator.fingerprint(remote, first),
            AndroidSessionSyncCoordinator.fingerprint(remote, reordered));
        assertNotEquals(AndroidSessionSyncCoordinator.fingerprint(remote, first),
            AndroidSessionSyncCoordinator.fingerprint(remote, different));
    }
}
