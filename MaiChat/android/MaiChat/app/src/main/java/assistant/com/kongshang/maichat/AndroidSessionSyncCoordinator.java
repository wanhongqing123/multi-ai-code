// 文件作用：Android 创作 Session 的设备目录、消息路由与所属设备操作分发。
// 同步服务只转发同账号设备消息；模型、工具和文件操作仍在本机 MaiAgent 执行。
package com.kongshang.maichat;

import android.content.Context;
import android.content.SharedPreferences;
import android.os.Build;
import android.util.Log;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.Arrays;
import java.util.Base64;
import java.util.Map;
import java.util.UUID;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.TimeUnit;
import java.util.function.BiConsumer;
import org.json.JSONArray;
import org.json.JSONObject;

final class AndroidSessionSyncCoordinator {
    private static final String TAG = "MaiChatSessionSync";
    private final Context context;
    private final AIAssistantController controller;
    private final MaiChatAuthClient auth;
    private final String slot;
    private final String deviceId;
    private final String deviceName;
    private final ExecutorService outbound = Executors.newSingleThreadExecutor();
    private final SessionSyncPayloadAssembler incoming = new SessionSyncPayloadAssembler();
    private final Map<String, String> retryIdByFingerprint = new ConcurrentHashMap<>();
    private final Map<String, String> fingerprintByCommandId = new ConcurrentHashMap<>();
    private volatile boolean closed;
    private Thread listener;
    private long lastPresence;

    AndroidSessionSyncCoordinator(Context context, AIAssistantController controller,
                                  MaiChatAuthClient auth) {
        this.context = context.getApplicationContext();
        this.controller = controller;
        this.auth = auth;
        slot = this.context.getResources().getConfiguration().smallestScreenWidthDp >= 600
            ? "tablet" : "android_phone";
        SharedPreferences settings = this.context.getSharedPreferences(
            "maichat-session-sync", Context.MODE_PRIVATE);
        String saved = settings.getString("device-id", "");
        if (!saved.matches("[a-f0-9]{32}")) {
            saved = UUID.randomUUID().toString().replace("-", "");
            settings.edit().putString("device-id", saved).apply();
        }
        deviceId = saved;
        String cleanName = (Build.MANUFACTURER + " " + Build.MODEL).trim()
            .replaceAll("[\\p{Cntrl}]", "");
        deviceName = cleanName.substring(0, Math.min(40, cleanName.length()));
    }

    void start() {
        listener = new Thread(this::listen, "MaiChat-SessionSync");
        listener.start();
    }

    void close() {
        closed = true;
        if (listener != null) listener.interrupt();
        outbound.shutdownNow();
    }

    private static String newId() { return UUID.randomUUID().toString().replace("-", ""); }

    private static String hex(byte[] bytes) {
        StringBuilder result = new StringBuilder(bytes.length * 2);
        for (byte value : bytes) result.append(String.format("%02x", value & 0xff));
        return result.toString();
    }

    private void listen() {
        while (!closed) {
            try {
                if (!controller.state.ready) {
                    Thread.sleep(2000);
                    continue;
                }
                if (System.currentTimeMillis() - lastPresence >= 15000) {
                    JSONArray sessions = controller.localSessionsForSync();
                    JSONArray entries = new JSONArray();
                    for (int i = 0; i < Math.min(200, sessions.length()); i++) {
                        JSONObject session = sessions.optJSONObject(i);
                        if (session == null) continue;
                        entries.put(new JSONObject().put("id", session.optString("id"))
                            .put("title", session.optString("title").replaceAll("[\\p{Cntrl}]", ""))
                            .put("busy", session.optBoolean("busy"))
                            .put("updated_at", System.currentTimeMillis() - i));
                    }
                    auth.sessionSyncRequest("presence", new JSONObject().put("device_id", deviceId)
                        .put("name", deviceName.isEmpty() ? "Android" : deviceName)
                        .put("sessions", entries), 0);
                    JSONObject catalog = auth.sessionSyncRequest("sessions", null, 0);
                    controller.acceptRemoteCatalog(catalog.optJSONArray("sessions"), slot);
                    lastPresence = System.currentTimeMillis();
                }
                JSONArray messages = auth.sessionSyncRequest("poll", null, 10000)
                    .optJSONArray("messages");
                if (messages != null) for (int i = 0; i < messages.length(); i++) {
                    JSONObject envelope = messages.optJSONObject(i);
                    if (envelope != null) handle(envelope);
                }
            } catch (InterruptedException interrupted) {
                Thread.currentThread().interrupt();
                return;
            } catch (Exception failure) {
                if (!closed) {
                    Log.w(TAG, "Device sync temporarily unavailable");
                    if (System.currentTimeMillis() - lastPresence > 45000)
                        controller.markRemoteCatalogOffline();
                    try { Thread.sleep(2000); }
                    catch (InterruptedException interrupted) {
                        Thread.currentThread().interrupt();
                        return;
                    }
                }
            }
        }
    }

    private void handle(JSONObject envelope) throws Exception {
        String kind = envelope.optString("kind");
        if (kind.equals("history_request")) {
            JSONObject request = envelope.optJSONObject("payload");
            int limit = request == null ? 10 : Math.max(1, Math.min(20, request.optInt("limit", 10)));
            String before = request == null ? "" : request.optString("before");
            CompletableFuture<JSONObject> page = new CompletableFuture<>();
            controller.historyForSync(envelope.optString("session_id"), before, limit, page::complete);
            sendPayload(envelope, "history_page", page.get(15, TimeUnit.SECONDS));
        } else if (kind.equals("command")) {
            CompletableFuture<JSONObject> result = new CompletableFuture<>();
            controller.commandForSync(envelope.optString("session_id"),
                envelope.optJSONObject("payload"), result::complete);
            sendPayload(envelope, "event", new JSONObject().put("type", "command_result")
                .put("command_id", envelope.optString("client_message_id"))
                .put("result", result.get(15, TimeUnit.SECONDS)));
        } else if (kind.equals("event") || kind.equals("history_page")) {
            acceptChunk(envelope);
        }
        auth.sessionSyncRequest("ack", new JSONObject().put("ids",
            new JSONArray().put(envelope.optLong("id"))), 0);
    }

    private void sendPayload(JSONObject envelope, String kind, JSONObject value) throws Exception {
        byte[] data = value.toString().getBytes(StandardCharsets.UTF_8);
        int count = Math.max(1, (data.length + 24 * 1024 - 1) / (24 * 1024));
        if (count > 128) throw new IllegalArgumentException("Session payload is too large");
        String transferId = newId();
        String digest = hex(MessageDigest.getInstance("SHA-256").digest(data));
        for (int index = 0; index < count; index++) {
            byte[] part = Arrays.copyOfRange(data, index * 24 * 1024,
                Math.min(data.length, (index + 1) * 24 * 1024));
            JSONObject chunk = new JSONObject().put("transfer_id", transferId)
                .put("chunk_index", index).put("chunk_count", count)
                .put("sha256", digest)
                .put("data_base64", Base64.getEncoder().encodeToString(part));
            send(envelope.optString("from_slot"), envelope.optString("from_login_session_id"),
                envelope.optString("session_id"), kind, chunk);
        }
    }

    private void acceptChunk(JSONObject envelope) throws Exception {
        JSONObject completed = incoming.accept(envelope);
        if (completed == null) return;
        if (completed.optString("type").equals("command_result")) {
            String id = completed.optString("command_id");
            String fingerprint = fingerprintByCommandId.remove(id);
            if (fingerprint != null) retryIdByFingerprint.remove(fingerprint);
        }
        controller.acceptRemotePayload(envelope, completed);
    }

    private JSONObject send(String toSlot, String toLogin, String session, String kind,
                            JSONObject payload) throws Exception {
        return send(toSlot, toLogin, session, kind, payload, newId());
    }

    private JSONObject send(String toSlot, String toLogin, String session, String kind,
                            JSONObject payload, String messageId) throws Exception {
        return auth.sessionSyncRequest("send", new JSONObject()
            .put("to_slot", toSlot).put("to_login_session_id", toLogin)
            .put("client_message_id", messageId).put("session_id", session)
            .put("kind", kind).put("payload", payload), 0);
    }

    static String fingerprint(JSONObject remote, JSONObject action) throws Exception {
        StringBuilder canonical = new StringBuilder();
        for (String value : new String[] {remote.optString("device_slot"),
            remote.optString("login_session_id"), remote.optString("id"),
            action.optString("action"), action.optString("text"),
            action.optString("id"), action.optString("decision"), action.optString("title")})
            canonical.append(value.length()).append(':').append(value);
        return hex(MessageDigest.getInstance("SHA-256").digest(
            canonical.toString().getBytes(StandardCharsets.UTF_8)));
    }

    String sendToRemote(JSONObject remote, String kind, JSONObject payload,
                        BiConsumer<String, String> completion) throws Exception {
        String id = newId();
        if (kind.equals("command")) {
            String key = fingerprint(remote, payload);
            id = retryIdByFingerprint.computeIfAbsent(key, ignored -> newId());
            fingerprintByCommandId.put(id, key);
        }
        final String messageId = id;
        if (closed) { completion.accept(messageId, "同步服务已停止"); return messageId; }
        try {
            outbound.execute(() -> {
                String error = "";
                try {
                    if (closed) throw new IllegalStateException("同步服务已停止");
                    send(remote.optString("device_slot"), remote.optString("login_session_id"),
                        remote.optString("id"), kind, payload, messageId);
                } catch (Exception failure) { error = failure.getMessage(); }
                completion.accept(messageId, error == null ? "设备同步失败" : error);
            });
        } catch (RejectedExecutionException stopped) {
            completion.accept(messageId, "同步服务已停止");
        }
        return messageId;
    }
}
