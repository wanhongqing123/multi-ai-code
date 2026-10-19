// 文件作用：校验并重组跨设备历史与事件分块，不解释模型或工具内容。
package com.kongshang.maichat;

import java.io.ByteArrayOutputStream;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.Base64;
import java.util.HashMap;
import java.util.Map;
import org.json.JSONObject;

final class SessionSyncPayloadAssembler {
    private static final int CHUNK_SIZE = 24 * 1024;
    private static final int MAX_CHUNKS = 128;
    private static final int MAX_BYTES = 3 * 1024 * 1024;
    private final Map<String, Transfer> incoming = new HashMap<>();

    private static final class Transfer {
        String slot, login, session, kind, digest;
        int count, received;
        long started;
        byte[][] parts;
    }

    JSONObject accept(JSONObject envelope) throws Exception {
        JSONObject payload = envelope.optJSONObject("payload");
        if (payload == null) return null;
        String transferId = payload.optString("transfer_id");
        String digest = payload.optString("sha256");
        int index = payload.optInt("chunk_index", -1);
        int count = payload.optInt("chunk_count", -1);
        if (transferId.isEmpty() || digest.length() != 64 || count < 1 || count > MAX_CHUNKS ||
            index < 0 || index >= count) return null;
        incoming.entrySet().removeIf(item -> System.currentTimeMillis() - item.getValue().started > 120000);
        if (incoming.size() >= 128 && !incoming.containsKey(transferId)) return null;
        Transfer transfer = incoming.get(transferId);
        if (transfer == null) {
            transfer = new Transfer();
            transfer.slot = envelope.optString("from_slot");
            transfer.login = envelope.optString("from_login_session_id");
            transfer.session = envelope.optString("session_id");
            transfer.kind = envelope.optString("kind");
            transfer.digest = digest;
            transfer.count = count;
            transfer.started = System.currentTimeMillis();
            transfer.parts = new byte[count][];
            incoming.put(transferId, transfer);
        }
        if (!transfer.slot.equals(envelope.optString("from_slot")) ||
            !transfer.login.equals(envelope.optString("from_login_session_id")) ||
            !transfer.session.equals(envelope.optString("session_id")) ||
            !transfer.kind.equals(envelope.optString("kind")) ||
            !transfer.digest.equals(digest) || transfer.count != count) return null;
        byte[] part;
        try { part = Base64.getDecoder().decode(payload.optString("data_base64")); }
        catch (IllegalArgumentException invalid) { return null; }
        if (part.length > CHUNK_SIZE) return null;
        if (transfer.parts[index] == null) transfer.received++;
        transfer.parts[index] = part;
        if (transfer.received < count) return null;
        incoming.remove(transferId);
        ByteArrayOutputStream combined = new ByteArrayOutputStream();
        for (byte[] piece : transfer.parts) {
            if (piece == null || combined.size() + piece.length > MAX_BYTES) return null;
            combined.write(piece);
        }
        byte[] bytes = combined.toByteArray();
        byte[] hash = MessageDigest.getInstance("SHA-256").digest(bytes);
        StringBuilder actual = new StringBuilder(hash.length * 2);
        for (byte value : hash) actual.append(String.format("%02x", value & 0xff));
        if (!actual.toString().equals(digest)) return null;
        return new JSONObject(new String(bytes, StandardCharsets.UTF_8));
    }
}
