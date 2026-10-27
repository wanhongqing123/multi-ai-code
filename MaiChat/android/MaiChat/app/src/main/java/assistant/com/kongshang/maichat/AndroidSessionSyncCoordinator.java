// 文件作用：Android 创作 Session 的设备目录、消息路由与所属设备操作分发。
// 同步服务只转发同账号设备消息；模型、工具和文件操作仍在本机 MaiAgent 执行。
package com.kongshang.maichat;

import android.content.Context;
import android.content.SharedPreferences;
import android.os.Build;
import android.util.Log;
import java.io.IOException;
import java.net.InetSocketAddress;
import java.net.Proxy;
import java.net.ProxySelector;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.util.Arrays;
import java.util.Base64;
import java.util.Set;
import java.util.Map;
import java.util.UUID;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.function.BiConsumer;
import org.json.JSONArray;
import org.json.JSONObject;

final class AndroidSessionSyncCoordinator {
    private static final String TAG = "MaiChatSessionSync";
    private static final String LIFECYCLE_SESSION = "session_lifecycle";
    private static final class PendingLifecycle {
        final String slot;
        final String login;
        final BiConsumer<JSONObject, String> completion;
        PendingLifecycle(String slot, String login, BiConsumer<JSONObject, String> completion) {
            this.slot = slot;
            this.login = login;
            this.completion = completion;
        }
    }
    private final Context context;
    private final AIAssistantController controller;
    private final MaiChatAuthClient auth;
    private final String slot;
    private final String deviceId;
    private final String deviceName;
    private final ExecutorService outbound = Executors.newSingleThreadExecutor();
    private final ScheduledExecutorService lifecycleTimeouts =
        Executors.newSingleThreadScheduledExecutor();
    private final SessionSyncPayloadAssembler incoming = new SessionSyncPayloadAssembler();
    private final BlockingQueue<JSONObject> mailbox = new LinkedBlockingQueue<>(1000);
    private final Map<String, CompletableFuture<JSONObject>> responses = new ConcurrentHashMap<>();
    private final Map<String, PendingLifecycle> pendingLifecycle = new ConcurrentHashMap<>();
    private final Map<String, String> retryIdByFingerprint = new ConcurrentHashMap<>();
    private final Map<String, String> fingerprintByCommandId = new ConcurrentHashMap<>();
    private final Set<String> dirtySessions = ConcurrentHashMap.newKeySet();
    private final Set<String> initiallyPublishedSessions = ConcurrentHashMap.newKeySet();
    private final Map<String, String> publishedMessageDigests = new ConcurrentHashMap<>();
    private final Map<String, String> publishedStateDigests = new ConcurrentHashMap<>();
    private final BlockingQueue<String> backfillSessions = new LinkedBlockingQueue<>(200);
    private final Map<String, String> backfillBefore = new ConcurrentHashMap<>();
    private final Object socketLock = new Object();
    private volatile boolean closed;
    private volatile boolean connected;
    private volatile CompletableFuture<Void> presenceReady = new CompletableFuture<>();
    private volatile long socketHandle;
    private Thread listener;
    private volatile long lastPresence;
    private volatile String localLoginSessionId = "";
    private long lastBackfill;

    private native long nativeCreate();
    private native boolean nativeStart(long handle, String url, String bearer,
                                       String caPem, String proxyUrl);
    private native boolean nativeSend(long handle, byte[] frame);
    private native boolean nativeUpdateBearer(long handle, String bearer);
    private native void nativeDestroy(long handle);

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
        presenceReady.completeExceptionally(new IOException("设备同步连接已关闭"));
        if (listener != null) listener.interrupt();
        outbound.shutdownNow();
        lifecycleTimeouts.shutdownNow();
        long handle;
        synchronized (socketLock) {
            handle = socketHandle;
            socketHandle = 0;
        }
        if (handle != 0) nativeDestroy(handle);
        failResponses("设备同步连接已关闭");
        failLifecycle("设备同步连接已关闭");
    }

    private static String newId() { return UUID.randomUUID().toString().replace("-", ""); }

    private static String hex(byte[] bytes) {
        StringBuilder result = new StringBuilder(bytes.length * 2);
        for (byte value : bytes) result.append(String.format("%02x", value & 0xff));
        return result.toString();
    }

    private void failResponses(String message) {
        for (CompletableFuture<JSONObject> response : responses.values())
            response.completeExceptionally(new IOException(message));
        responses.clear();
    }

    private void finishLifecycle(String id, JSONObject result, String error) {
        PendingLifecycle pending = pendingLifecycle.remove(id);
        if (pending != null) pending.completion.accept(result, error);
    }

    private void failLifecycle(String error) {
        for (String id : pendingLifecycle.keySet()) finishLifecycle(id, null, error);
    }

    private void onNativeSocketEvent(int event, byte[] bytes) {
        if (closed) return;
        if (event == 1) {
            lastPresence = 0;
            localLoginSessionId = "";
            presenceReady = new CompletableFuture<>();
            connected = true;
            return;
        }
        if (event == 2 || event == 4) {
            connected = false;
            lastPresence = 0;
            localLoginSessionId = "";
            presenceReady.completeExceptionally(new IOException("设备同步连接中断"));
            initiallyPublishedSessions.clear();
            publishedMessageDigests.clear();
            publishedStateDigests.clear();
            backfillSessions.clear();
            backfillBefore.clear();
            failResponses("设备同步连接中断");
            failLifecycle("设备同步连接中断，请检查所属设备的会话列表");
            controller.markRemoteCatalogOffline();
            return;
        }
        if (event != 3 || bytes == null) return;
        try {
            JSONObject frame = new JSONObject(new String(bytes, StandardCharsets.UTF_8));
            String kind = frame.optString("type");
            if (kind.equals("response")) {
                CompletableFuture<JSONObject> response = responses.remove(frame.optString("id"));
                if (response != null) response.complete(frame);
            } else if (kind.equals("catalog")) {
                controller.acceptRemoteCatalog(frame.optJSONArray("sessions"),
                    frame.optJSONArray("devices"), slot);
            } else if (kind.equals("inbox")) {
                JSONArray messages = frame.optJSONArray("messages");
                if (messages != null) for (int i = 0; i < messages.length(); i++) {
                    JSONObject message = messages.optJSONObject(i);
                    if (message != null && LIFECYCLE_SESSION.equals(
                        message.optString("session_id")) &&
                        message.optString("kind").equals("event"))
                        acceptLifecycleResult(message);
                    if (message != null && !mailbox.offer(message))
                        Log.w(TAG, "Device sync inbox is full");
                }
            }
        } catch (Exception failure) { Log.w(TAG, "Invalid device sync frame"); }
    }

    private JSONObject request(String operation, JSONObject body, int timeoutMs) throws Exception {
        if (!connected) throw new IOException("所属设备同步连接尚未建立");
        if (!operation.equals("presence")) {
            try { presenceReady.get(timeoutMs, TimeUnit.MILLISECONDS); }
            catch (InterruptedException interrupted) {
                Thread.currentThread().interrupt();
                throw interrupted;
            }
            catch (Exception failure) {
                throw new IOException("所属设备同步正在重新登记，请稍后重试", failure);
            }
        }
        String id = newId();
        CompletableFuture<JSONObject> future = new CompletableFuture<>();
        responses.put(id, future);
        JSONObject frame = new JSONObject().put("id", id).put("op", operation)
            .put("body", body == null ? new JSONObject() : body);
        byte[] bytes = frame.toString().getBytes(StandardCharsets.UTF_8);
        boolean sent;
        synchronized (socketLock) {
            sent = socketHandle != 0 && nativeSend(socketHandle, bytes);
        }
        if (!sent) {
            responses.remove(id);
            throw new IOException("设备同步连接中断");
        }
        try {
            JSONObject response = future.get(timeoutMs, TimeUnit.MILLISECONDS);
            if (response.optInt("status") != 200) {
                JSONObject result = response.optJSONObject("body");
                throw new IOException(result == null ? "设备同步请求失败" :
                    result.optString("error", "设备同步请求失败"));
            }
            JSONObject result = response.optJSONObject("body");
            return result == null ? new JSONObject() : result;
        } finally { responses.remove(id); }
    }

    private String systemProxy() throws IOException {
        ProxySelector selector = ProxySelector.getDefault();
        if (selector == null) return "";
        java.util.List<Proxy> proxies = selector.select(URI.create("https://ichat.life/"));
        if (proxies == null || proxies.isEmpty() || proxies.get(0).type() == Proxy.Type.DIRECT)
            return "";
        Proxy proxy = proxies.get(0);
        if (proxy.type() != Proxy.Type.HTTP || !(proxy.address() instanceof InetSocketAddress))
            throw new IOException("当前网络代理不支持设备同步");
        InetSocketAddress address = (InetSocketAddress) proxy.address();
        String host = address.getHostString();
        if (host.contains(":")) host = "[" + host + "]";
        return "http://" + host + ":" + address.getPort() + "/";
    }

    private void listen() {
        while (!closed) {
            try {
                if (!controller.state.ready) {
                    Thread.sleep(2000);
                    continue;
                }
                if (socketHandle == 0) {
                    String token = auth.sessionSyncAccessToken();
                    String certificate = controller.sessionSyncCertificatePem();
                    String proxy = systemProxy();
                    synchronized (socketLock) {
                        if (closed) return;
                        long handle = nativeCreate();
                        if (handle == 0) throw new IOException("设备同步连接初始化失败");
                        if (!nativeStart(handle, "wss://ichat.life/api/v1/session-sync/ws",
                            token, certificate, proxy)) {
                            nativeDestroy(handle);
                            throw new IOException("设备同步连接启动失败");
                        }
                        socketHandle = handle;
                    }
                }
                if (!connected) {
                    String refreshed = auth.sessionSyncAccessToken();
                    synchronized (socketLock) {
                        if (socketHandle != 0)
                            nativeUpdateBearer(socketHandle, refreshed);
                    }
                    Thread.sleep(1500);
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
                    String refreshed = auth.sessionSyncAccessToken();
                    synchronized (socketLock) {
                        if (socketHandle != 0)
                            nativeUpdateBearer(socketHandle, refreshed);
                    }
                    JSONObject presence = request("presence", new JSONObject().put("device_id", deviceId)
                        .put("name", deviceName.isEmpty() ? "Android" : deviceName)
                        .put("sessions", entries), 15000);
                    localLoginSessionId = presence.optString("login_session_id");
                    presenceReady.complete(null);
                    Set<String> currentIds = new java.util.HashSet<>();
                    for (int i = 0; i < entries.length(); i++) {
                        String id = entries.getJSONObject(i).optString("id");
                        currentIds.add(id);
                        if (initiallyPublishedSessions.add(id)) {
                            dirtySessions.add(id);
                            backfillSessions.offer(id);
                        }
                    }
                    for (String removed : new java.util.HashSet<>(initiallyPublishedSessions)) {
                        if (currentIds.contains(removed)) continue;
                        request("room-delete", new JSONObject().put("session_id", removed), 15000);
                        try {
                            new MaiSessionAttachments(context).cleanupRoom(
                                new MaiSessionAttachments.Room(slot,localLoginSessionId,removed));
                        } catch (Exception failure) {
                            Log.w(TAG,"Session attachment cleanup will retry");
                            continue;
                        }
                        initiallyPublishedSessions.remove(removed);
                        dirtySessions.remove(removed);
                        backfillSessions.remove(removed);
                        backfillBefore.remove(removed);
                        publishedStateDigests.remove(removed);
                        publishedMessageDigests.keySet().removeIf(
                            key -> key.startsWith(removed + ":"));
                    }
                    lastPresence = System.currentTimeMillis();
                }
                JSONObject envelope = mailbox.poll(1000, TimeUnit.MILLISECONDS);
                if (envelope != null) handle(envelope);
                for (String session : dirtySessions) {
                    publishSession(session, "");
                    dirtySessions.remove(session);
                }
                if (System.currentTimeMillis() - lastBackfill >= 5000 &&
                    !backfillSessions.isEmpty()) {
                    String session = backfillSessions.peek();
                    String next;
                    if (!backfillBefore.containsKey(session)) {
                        CompletableFuture<JSONObject> first = new CompletableFuture<>();
                        controller.historyForSync(session, "", 20, first::complete);
                        JSONArray recent = first.get(15, TimeUnit.SECONDS).optJSONArray("messages");
                        JSONObject oldest = recent == null ? null :
                            recent.optJSONObject(recent.length() - 1);
                        next = recent != null && recent.length() == 20 && oldest != null
                            ? oldest.optString("id") : null;
                    } else next = publishSession(session, backfillBefore.get(session));
                    if (next == null || next.isEmpty()) {
                        backfillSessions.poll();
                        backfillBefore.remove(session);
                    } else backfillBefore.put(session, next);
                    lastBackfill = System.currentTimeMillis();
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
        if (kind.equals("command") && LIFECYCLE_SESSION.equals(envelope.optString("session_id"))) {
            CompletableFuture<JSONObject> result = new CompletableFuture<>();
            controller.lifecycleForSync(envelope.optJSONObject("payload"), result::complete);
            send(envelope.optString("from_slot"), envelope.optString("from_login_session_id"),
                LIFECYCLE_SESSION, "event", new JSONObject().put("type", "command_result")
                    .put("command_id", envelope.optString("client_message_id"))
                    .put("result", result.get(15, TimeUnit.SECONDS)));
            requestPresenceRefresh();
        } else if (kind.equals("command")) {
            CompletableFuture<JSONObject> result = new CompletableFuture<>();
            controller.commandForSync(envelope.optString("session_id"),
                envelope.optJSONObject("payload"), result::complete);
            sendPayload(envelope, "event", new JSONObject().put("type", "command_result")
                .put("command_id", envelope.optString("client_message_id"))
                .put("result", result.get(15, TimeUnit.SECONDS)));
            schedulePublish(envelope.optString("session_id"));
        } else if (kind.equals("event") && LIFECYCLE_SESSION.equals(envelope.optString("session_id"))) {
            acceptLifecycleResult(envelope);
        } else if (kind.equals("event")) {
            acceptChunk(envelope);
        }
        request("ack", new JSONObject().put("ids",
            new JSONArray().put(envelope.optLong("id"))), 15000);
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

    void schedulePublish(String sessionId) {
        if (sessionId != null && !sessionId.isEmpty()) dirtySessions.add(sessionId);
    }

    void requestPresenceRefresh() { lastPresence = 0; }

    private void acceptLifecycleResult(JSONObject envelope) {
        JSONObject completed = envelope.optJSONObject("payload");
        if (completed == null || !completed.optString("type").equals("command_result")) return;
        String id = completed.optString("command_id");
        PendingLifecycle pending = pendingLifecycle.get(id);
        if (pending == null || !pending.slot.equals(envelope.optString("from_slot")) ||
            !pending.login.equals(envelope.optString("from_login_session_id"))) return;
        JSONObject result = completed.optJSONObject("result");
        finishLifecycle(id, result, result == null ? "所属设备未返回执行结果" : "");
    }

    void sendLifecycle(JSONObject device, JSONObject action,
                       BiConsumer<JSONObject, String> completion) {
        String toSlot = device.optString("device_slot");
        String toLogin = device.optString("login_session_id");
        if (closed || !device.optBoolean("online") || toSlot.isEmpty() || toLogin.isEmpty()) {
            completion.accept(null, "所属设备离线或同步服务不可用");
            return;
        }
        String id = newId();
        pendingLifecycle.put(id, new PendingLifecycle(toSlot, toLogin, completion));
        try {
            lifecycleTimeouts.schedule(() -> finishLifecycle(id, null,
                "所属设备执行状态未知，请先查看会话列表，避免重复操作"), 30, TimeUnit.SECONDS);
            outbound.execute(() -> {
                try {
                    send(toSlot, toLogin, LIFECYCLE_SESSION, "command", action, id);
                } catch (Exception failure) {
                    finishLifecycle(id, null, failure.getMessage() == null
                        ? "设备同步请求失败" : failure.getMessage());
                }
            });
        } catch (RejectedExecutionException stopped) {
            finishLifecycle(id, null, "设备同步服务已停止");
        }
    }

    private String publishSession(String sessionId, String before) throws Exception {
        CompletableFuture<JSONObject> state = new CompletableFuture<>();
        controller.historyForSync(sessionId, before, 20, state::complete);
        JSONObject page = state.get(15, TimeUnit.SECONDS);
        if (!page.optBoolean("ok")) return null;
        JSONArray messages = page.optJSONArray("messages");
        JSONArray permissions = page.optJSONArray("permissions");
        JSONArray questions = page.optJSONArray("questions");
        if (messages == null) messages = new JSONArray();
        if (permissions == null) permissions = new JSONArray();
        if (questions == null) questions = new JSONArray();
        String stateDigest = hex(MessageDigest.getInstance("SHA-256").digest(
            (permissions.toString() + questions.toString()).getBytes(StandardCharsets.UTF_8)));
        boolean sentMessage = false;
        for (int i = 0; i < messages.length(); i++) {
            JSONObject message = messages.optJSONObject(i);
            if (message == null) continue;
            String key = sessionId + ":" + message.optString("id");
            String digest = hex(MessageDigest.getInstance("SHA-256").digest(
                message.toString().getBytes(StandardCharsets.UTF_8)));
            if (digest.equals(publishedMessageDigests.get(key))) continue;
            request("publish", new JSONObject().put("session_id", sessionId)
                .put("messages", new JSONArray().put(message))
                .put("permissions", permissions).put("questions", questions), 15000);
            publishedMessageDigests.put(key, digest);
            publishedStateDigests.put(sessionId, stateDigest);
            sentMessage = true;
        }
        if (!sentMessage && !stateDigest.equals(publishedStateDigests.get(sessionId))) {
            request("publish", new JSONObject().put("session_id", sessionId)
                .put("messages", new JSONArray())
                .put("permissions", permissions).put("questions", questions), 15000);
            publishedStateDigests.put(sessionId, stateDigest);
        }
        JSONObject oldest = messages.optJSONObject(messages.length() - 1);
        return messages.length() == 20 && oldest != null ? oldest.optString("id") : null;
    }

    String localSlot() { return slot; }
    String localLoginSessionId() { return localLoginSessionId; }

    void publishAttachedMessage(String sessionId, JSONObject message,
                                JSONArray permissions, JSONArray questions) {
        if (closed) return;
        outbound.execute(() -> {
            for (int attempt = 0; attempt < 5 && !closed; attempt++) {
                try {
                    request("publish",new JSONObject().put("session_id",sessionId)
                        .put("messages",new JSONArray().put(message))
                        .put("permissions",permissions == null ? new JSONArray() : permissions)
                        .put("questions",questions == null ? new JSONArray() : questions),15000);
                    return;
                } catch (Exception failure) {
                    if (attempt == 4) Log.w(TAG,"Session attachment history sync failed");
                    else try { Thread.sleep(2000); }
                    catch (InterruptedException stopped) { Thread.currentThread().interrupt(); return; }
                }
            }
        });
    }

    void readRemoteHistory(JSONObject remote, String before, int limit,
                           BiConsumer<JSONObject, String> completion) {
        try {
            outbound.execute(() -> {
                JSONObject page = null;
                String error = "";
                try {
                    page = request("history", new JSONObject()
                        .put("owner_slot", remote.optString("device_slot"))
                        .put("session_id", remote.optString("id"))
                        .put("before", before).put("limit", limit), 15000);
                } catch (Exception failure) { error = failure.getMessage(); }
                completion.accept(page, error == null ? "设备同步失败" : error);
            });
        } catch (RejectedExecutionException stopped) {
            completion.accept(null, "设备同步服务已停止");
        }
    }

    private JSONObject send(String toSlot, String toLogin, String session, String kind,
                            JSONObject payload) throws Exception {
        return send(toSlot, toLogin, session, kind, payload, newId());
    }

    private JSONObject send(String toSlot, String toLogin, String session, String kind,
                            JSONObject payload, String messageId) throws Exception {
        return request("send", new JSONObject()
            .put("to_slot", toSlot).put("to_login_session_id", toLogin)
            .put("client_message_id", messageId).put("session_id", session)
            .put("kind", kind).put("payload", payload), 15000);
    }

    static String fingerprint(JSONObject remote, JSONObject action) throws Exception {
        StringBuilder canonical = new StringBuilder();
        for (String value : new String[] {remote.optString("device_slot"),
            remote.optString("login_session_id"), remote.optString("id"),
            action.optString("action"), action.optString("text"),
            action.optString("id"), action.optString("decision"), action.optString("title"),
            action.optJSONArray("attachments") == null ? "" :
                action.optJSONArray("attachments").toString()})
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
