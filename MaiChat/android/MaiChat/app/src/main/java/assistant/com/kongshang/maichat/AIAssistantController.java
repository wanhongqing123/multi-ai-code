// 文件作用：Android 创作中心的会话、消息和 Agent 交互。
// AIAssistantController 负责流程编排、异步回调和状态转换。
// 主要入口：shared、configureAccount、invalidateSharedTemporaryModelCredentials、nativeCreate。
package com.kongshang.maichat;

import android.content.Context;
import android.database.Cursor;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Matrix;
import android.media.ExifInterface;
import android.net.Uri;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.provider.OpenableColumns;
import android.util.Base64;
import android.util.Log;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.ByteBuffer;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.nio.file.AtomicMoveNotSupportedException;
import java.security.KeyStore;
import java.security.MessageDigest;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.function.Consumer;
import org.json.JSONArray;
import org.json.JSONObject;

/** Native core, JSON, SQLite, credentials and imports are confined to the named worker. */
public final class AIAssistantController {
    interface HostToolHandler {
        Object execute(String toolName, JSONObject arguments) throws Exception;
    }
    interface Listener {
        void onState(State state);
    }
    static final class State {
        final JSONObject data;
        final JSONObject remoteSession;
        final String selected, baseUrl, model, policy, error, syncStatus;
        final boolean ready;
        final int messageHash, messageLength;
        State(JSONObject data, JSONObject remoteSession, String selected, String baseUrl,
              String model, String policy, String error, String syncStatus, boolean ready) {
            this.data = data;
            this.remoteSession = remoteSession;
            this.selected = selected;
            this.baseUrl = baseUrl;
            this.model = model;
            this.policy = policy;
            this.error = error;
            this.syncStatus = syncStatus;
            this.ready = ready;
            // 在工作线程计算可见消息指纹，UI 只在正文或工具状态变化时重绑列表。
            JSONArray messages = data.optJSONArray("messages");
            String encoded = messages == null ? "" : messages.toString();
            this.messageHash = encoded.hashCode();
            this.messageLength = encoded.length();
        }
        boolean busy() {
            if (remoteSession != null) return remoteSession.optBoolean("busy");
            JSONArray sessions = data.optJSONArray("sessions");
            if (sessions != null)
                for (int i = 0; i < sessions.length(); i++) {
                    JSONObject s = sessions.optJSONObject(i);
                    if (s != null && selected.equals(s.optString("id")))
                        return s.optBoolean("busy");
                }
            return false;
        }
    }
    public static final class ImportedFile {
        public final String relativePath;
        public final String mimeType;
        final String displayName;
        final boolean image;
        ImportedFile(String relativePath, String mimeType, boolean image) {
            this(relativePath, mimeType, image, relativePath);
        }
        ImportedFile(String relativePath, String mimeType, boolean image, String displayName) {
            this.relativePath = relativePath;
            this.mimeType = mimeType;
            this.image = image;
            this.displayName = displayName;
        }
    }
    private static AIAssistantController shared;
    private static String currentAccountUserId = "";
    private static Context applicationContext;
    public static synchronized AIAssistantController shared(Context context) {
        applicationContext = context.getApplicationContext();
        if (shared == null)
            shared = new AIAssistantController(context, null);
        return shared;
    }
    public static synchronized void configureAccount(String userId) {
        String next = userId == null ? "" : userId.trim();
        if (next.equals(currentAccountUserId)) return;
        String legacyOwner = currentAccountUserId.isEmpty() ? next : currentAccountUserId;
        if (applicationContext != null && !legacyOwner.isEmpty()) {
            try { claimLegacyWorkspace(applicationContext, legacyOwner); }
            catch (IOException failure) {
                // 标记失败时新账号使用独立目录，不得误认领旧账号的 AI 数据库。
                Log.w("MaiChatAgent", "Legacy AI workspace ownership was not saved", failure);
            }
        }
        if (shared != null) {
            shared.close();
            shared = null;
        }
        currentAccountUserId = next;
    }
    public static synchronized void invalidateSharedTemporaryModelCredentials() {
        if (shared == null) return;
        shared.worker.post(() -> {
            shared.arkApiKey = "";
            shared.arkExpiresAt = 0;
            shared.nextArkRefreshAt = 0;
            shared.wanApiKey = "";
            shared.wanExpiresAt = 0;
            shared.nextWanRefreshAt = 0;
            shared.wanWorkspaceId = "";
        });
    }
    private static native long nativeCreate();
    private static native void nativeDestroy(long handle);
    private static native byte[] nativeRequest(long handle, byte[] request);
    static native byte[] nativeMatteVideo(byte[] arguments, byte[] workspace,
        byte[] modelPath, byte[] runtimePath);
    private static native boolean nativeSetHostToolHandler(long handle, AIAssistantController owner);
    private static native byte[] nativeFilterBitmap(Bitmap bitmap, String operation, int[] dimensions);

    private final Context context;
    private final HandlerThread thread = new HandlerThread("MaiChat-Agent");
    private final Handler worker;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final String testEndpoint;
    private final String accountUserId;
    private final MaiChatAuthClient accountAuth;
    private final AndroidSessionSyncCoordinator sessionSync;
    private volatile Listener listener;
    private volatile HostToolHandler hostToolHandler;
    static final String GLM_CHAT_URL = "https://open.bigmodel.cn/api/coding/paas/v4";
    static final String GLM_RESPONSES_URL = "https://open.bigmodel.cn/api/v1";
    static final String DEEPSEEK_URL = "https://api.deepseek.com";
    static final String DEFAULT_MODEL = "deepseek-flash";
    volatile State state = new State(new JSONObject(), null, "", DEEPSEEK_URL,
        DEFAULT_MODEL, "on-request", "", "", false);
    private long handle;
    private File root;
    private File rvmModelFile;
    private volatile String selected = "", baseUrl = state.baseUrl, model = state.model, policy = state.policy,
                   error = "", apiKey = "", wire = "responses";
    private volatile String deepseekApiKey = "";
    private volatile String glmApiKey = "";
    private volatile String arkApiKey = "";
    private volatile long arkExpiresAt;
    private long nextArkRefreshAt;
    private volatile String wanApiKey = "";
    private volatile long wanExpiresAt;
    private long nextWanRefreshAt;
    private volatile String wanWorkspaceId = "";
    private volatile String miniMaxApiKey = "";
    private volatile JSONObject data = new JSONObject();
    private JSONArray remoteSessions = new JSONArray();
    private JSONObject selectedRemote;
    private JSONObject pendingRemoteSelection;
    private int remoteSelectionGeneration;
    private JSONObject remoteData = new JSONObject();
    private String remoteCommandStatus = "";
    private int pendingRemoteCommands;
    private int remoteCommandGeneration;
    private final Map<String, JSONArray> mediaMentionCache = new HashMap<>();
    private boolean ready;
    private volatile boolean closed;
    private final Runnable poll = new Runnable() {
        @Override
        public void run() {
            if (closed || listener == null)
                return;
            if (ready) {
                refreshSeedanceCredential(false);
                refreshBailianCredential(false);
                refresh(false);
            }
            worker.postDelayed(this, data.optBoolean("busy") ? 100 : 500);
        }
    };

    AIAssistantController(Context context, String testEndpoint) {
        this.context = context.getApplicationContext();
        accountAuth = MaiChatAuthClient.shared(this.context);
        if (testEndpoint != null && !BuildConfig.DEBUG)
            throw new IllegalArgumentException("Test endpoint in release build");
        this.testEndpoint = testEndpoint;
        accountUserId = testEndpoint == null ? currentAccountUserId : "";
        thread.start();
        worker = new Handler(thread.getLooper());
        worker.post(this::initialize);
        sessionSync = testEndpoint == null && !accountUserId.isEmpty()
            ? new AndroidSessionSyncCoordinator(this.context, this, accountAuth) : null;
        if (sessionSync != null) sessionSync.start();
    }
    void setListener(Listener next) {
        listener = next;
        worker.post(() -> {
            worker.removeCallbacks(poll);
            if (next != null && !closed) {
                publishState();
                poll.run();
            }
        });
    }
    void clearListener(Listener expected) {
        if (listener == expected)
            setListener(null);
    }
    void setHostToolHandler(HostToolHandler handler) {
        hostToolHandler = handler;
    }
    @SuppressWarnings("unused") // Called from MaiAgentJni.cpp on an agent worker thread.
    // 原生 Agent 工具回调在这里按名称分派到手机宿主。
    // 解析失败和权限拒绝都编码为可回传错误。
    private byte[] onNativeHostTool(byte[] toolBytes, byte[] argumentsBytes) {
        String tool = new String(toolBytes, StandardCharsets.UTF_8);
        String arguments = new String(argumentsBytes, StandardCharsets.UTF_8);
        if (tool.equals("mobile_decode_text")) {
            try {
                JSONObject request = new JSONObject(arguments);
                byte[] bytes = Base64.decode(request.getString("base64"), Base64.DEFAULT);
                if (bytes.length > 8 * 1024 * 1024)
                    return hostToolFailure("file exceeds the 8 MB text limit", "invalid_input");
                String decoded = AIAssistantTextDecoder.decode(bytes, request.getString("encoding"));
                return new JSONObject().put("ok", true).put("output", decoded)
                    .toString().getBytes(StandardCharsets.UTF_8);
            } catch (Throwable failure) {
                return hostToolFailure(safeMessage(failure), "invalid_input");
            }
        }
        if (tool.equals("ark_api_key")) {
            try {
                return new JSONObject().put("ok", true)
                    .put("output", new JSONObject().put("key",
                        System.currentTimeMillis() / 1000L < arkExpiresAt ? arkApiKey : ""))
                    .toString().getBytes(StandardCharsets.UTF_8);
            } catch (Exception failure) {
                return hostToolFailure("Ark key is unavailable", "not_configured");
            }
        }
        if (tool.equals("mobile_ark_service_config")) {
            try {
                MaiChatAuthClient.MediaServiceAccess access = accountAuth.mediaServiceAccess();
                if (access == null)
                    return hostToolFailure("Account login is required", "not_configured");
                return new JSONObject().put("ok", true)
                    .put("output", new JSONObject().put("base_url", access.baseUrl)
                        .put("token", access.accountToken))
                    .toString().getBytes(StandardCharsets.UTF_8);
            } catch (Exception failure) {
                return hostToolFailure("Ark Assets service is not configured", "not_configured");
            }
        }
        if (tool.equals("glm_api_key")) {
            try {
                return new JSONObject().put("ok", true)
                    .put("output", new JSONObject().put("key", glmApiKey))
                    .toString().getBytes(StandardCharsets.UTF_8);
            } catch (Exception failure) {
                return hostToolFailure("GLM key is unavailable", "not_configured");
            }
        }
        if (tool.equals("wan_credentials")) {
            try {
                return new JSONObject().put("ok", true)
                    .put("output", new JSONObject().put("key",
                        System.currentTimeMillis() / 1000L < wanExpiresAt ? wanApiKey : "")
                        .put("workspace_id",
                            System.currentTimeMillis() / 1000L < wanExpiresAt
                                ? wanWorkspaceId : ""))
                    .toString().getBytes(StandardCharsets.UTF_8);
            } catch (Exception failure) {
                return hostToolFailure("Wan credentials are unavailable", "not_configured");
            }
        }
        if (tool.equals("minimax_api_key")) {
            try {
                return new JSONObject().put("ok", true)
                    .put("output", new JSONObject().put("key", miniMaxApiKey))
                    .toString().getBytes(StandardCharsets.UTF_8);
            } catch (Exception failure) {
                return hostToolFailure("Creative model key is unavailable", "not_configured");
            }
        }
        HostToolHandler target = hostToolHandler;
        if (target == null)
            return hostToolFailure("MaiChat 宿主工具尚未连接");

        final byte[][] response = new byte[1][];
        Runnable invoke = () -> {
            try {
                Object output = target.execute(tool, new JSONObject(arguments));
                response[0] = new JSONObject().put("ok", true).put("output", output)
                    .toString().getBytes(StandardCharsets.UTF_8);
            } catch (Throwable failure) {
                response[0] = hostToolFailure(safeMessage(failure));
            }
        };
        if (tool.startsWith("mobile_") || tool.equals("generate_pdf")) {
            if (Looper.myLooper() == Looper.getMainLooper())
                return hostToolFailure("此宿主工具不能在 UI 线程执行");
            invoke.run();
            return response[0];
        }
        if (Looper.myLooper() == Looper.getMainLooper()) {
            invoke.run();
            return response[0];
        }
        CountDownLatch completed = new CountDownLatch(1);
        main.post(() -> {
            try { invoke.run(); }
            finally { completed.countDown(); }
        });
        try {
            completed.await();
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
            return hostToolFailure("MaiChat 宿主工具执行被中断");
        }
        return response[0] == null ? hostToolFailure("MaiChat 宿主工具未返回结果") : response[0];
    }
    private static byte[] hostToolFailure(String message) {
        return hostToolFailure(message, "internal");
    }
    private static byte[] hostToolFailure(String message, String code) {
        try {
            return new JSONObject().put("ok", false).put("error", message).put("errorCode", code)
                .toString().getBytes(StandardCharsets.UTF_8);
        } catch (Exception ignored) {
            return "{\"ok\":false,\"error\":\"MaiChat host tool failed\"}"
                .getBytes(StandardCharsets.UTF_8);
        }
    }
    // 作用：初始化控制器依赖、回调和原生 Agent 会话。
    private void initialize() {
        try {
            System.loadLibrary("maichat_agent");
            handle = nativeCreate();
            if (!nativeSetHostToolHandler(handle, this))
                throw new IllegalStateException("无法注册 MaiChat 宿主工具");
            root = testEndpoint == null ? accountRoot()
                : new File(context.getNoBackupFilesDir(), "AIAssistantTest-" + UUID.randomUUID());
            if (!root.mkdirs() && !root.isDirectory())
                throw new IllegalStateException("无法创建 AI 工作区");
            try {
                rvmModelFile = prepareBundledModel("rvm_mobilenetv3_fp32.onnx",
                    "88d4531297118f595bf2fd60f6f566aec2e559393802d1f436c380f0cbbd2828");
            }
            catch (Exception failure) { Log.w("MaiChatAgent", "Agent model staging failed", failure); }
            File settings = new File(root, "settings.json");
            clearLegacyModelKeyFiles();
            if (settings.isFile()) {
                JSONObject saved =
                    new JSONObject(new String(Files.readAllBytes(settings.toPath()), StandardCharsets.UTF_8));
                baseUrl = saved.optString("baseUrl", baseUrl);
                model = saved.optString("model", model);
                policy = saved.optString("policy", policy);
                // Workspace ID 已由服务端管理；升级时清除旧 settings.json 中的副本。
                if (saved.has("wanWorkspaceId")) {
                    saved.remove("wanWorkspaceId");
                    android.util.AtomicFile clean = new android.util.AtomicFile(settings);
                    FileOutputStream out = clean.startWrite();
                    try {
                        out.write(saved.toString().getBytes(StandardCharsets.UTF_8));
                        clean.finishWrite(out);
                    } catch (Exception failure) {
                        clean.failWrite(out);
                        Log.w("MaiChatAgent", "Legacy workspace ID cleanup failed", failure);
                    }
                }
            }
            model = supportedModelOrDefault(model);
            wire = "responses";
            if (model.equals("deepseek-flash")) baseUrl = DEEPSEEK_URL;
            else baseUrl = GLM_RESPONSES_URL;
            if (testEndpoint == null) {
                try {
                    refreshAccountModelKeys();
                } catch (Exception failure) {
                    Log.w("MaiChatAgent", "Account model keys are unavailable");
                }
            }
            refreshSeedanceCredential(true);
            refreshBailianCredential(true);
            apiKey = model.equals("deepseek-flash") ? deepseekApiKey : glmApiKey;
            if (testEndpoint != null) {
                baseUrl = testEndpoint;
                model = "test-model";
                wire = "chat_completions";
                apiKey = "test-key";
            }
            exportSystemCertificates();
            configure(baseUrl, model, policy, apiKey, wire);
            ready = true;
            refresh(true);
            JSONArray sessions = data.optJSONArray("sessions");
            if (sessions != null && sessions.length() > 0) {
                selected = sessions.getJSONObject(0).getString("id");
                refresh(true);
            }
        } catch (Throwable failure) {
            error = safeMessage(failure);
            publishState();
        }
    }
    static String supportedModelOrDefault(String selected) {
        return "glm-5.3".equals(selected) || "glm-5.3-flash".equals(selected) ||
            "deepseek-flash".equals(selected) ? selected : DEFAULT_MODEL;
    }
    private static String safeMessage(Throwable error) {
        String detail = error.getMessage();
        return detail == null || detail.trim().isEmpty() ? "创作中心操作失败" : detail;
    }
    private static void claimLegacyWorkspace(Context context, String userId) throws IOException {
        File base = new File(context.getNoBackupFilesDir(), "AIAssistant");
        File marker = new File(base, "legacy-owner-user-id.txt");
        if (!marker.isFile() && new File(base, "sessions.sqlite").isFile())
            Files.write(marker.toPath(), userId.getBytes(StandardCharsets.UTF_8));
    }
    private File accountRoot() throws Exception {
        File base = new File(context.getNoBackupFilesDir(), "AIAssistant");
        if (!base.mkdirs() && !base.isDirectory())
            throw new IOException("无法创建 AI 工作区");
        if (accountUserId.isEmpty()) return new File(base, "Guest");
        File marker = new File(base, "legacy-owner-user-id.txt");
        String legacyOwner = marker.isFile()
            ? new String(Files.readAllBytes(marker.toPath()), StandardCharsets.UTF_8).trim() : "";
        if (accountUserId.equals(legacyOwner)) return base;
        byte[] digest = MessageDigest.getInstance("SHA-256")
            .digest(accountUserId.getBytes(StandardCharsets.UTF_8));
        StringBuilder folder = new StringBuilder(digest.length * 2);
        for (byte value : digest) {
            folder.append(Character.forDigit((value >>> 4) & 15, 16));
            folder.append(Character.forDigit(value & 15, 16));
        }
        return new File(new File(base, "Accounts"), folder.toString());
    }
    private JSONObject invokeNativeAgent(JSONObject request) throws Exception {
        if (Looper.myLooper() == Looper.getMainLooper())
            throw new IllegalStateException("Native agent on UI thread");
        if (handle == 0)
            throw new IllegalStateException("AI 核心尚未初始化");
        byte[] response = nativeRequest(handle, request.toString().getBytes(StandardCharsets.UTF_8));
        if (response == null)
            throw new IllegalStateException("AI 核心未返回结果");
        JSONObject result = new JSONObject(new String(response, StandardCharsets.UTF_8));
        if (!result.optBoolean("ok"))
            throw new IllegalStateException(result.optString("error", "操作失败"));
        return result;
    }
    private JSONObject makeOperationRequest(String operationName) throws Exception {
        return new JSONObject().put("op", operationName).put("session", selected);
    }
    private void configure(String url, String name, String approval, String key,
                           String selectedWire) throws Exception {
        File workspace = new File(root, "Workspace");
        if (!workspace.mkdirs() && !workspace.isDirectory())
            throw new IllegalStateException("无法创建工作区");
        File runtime = new File(context.getApplicationInfo().nativeLibraryDir,
            "libonnxruntime.so");
        invokeNativeAgent(makeOperationRequest("configure")
                .put("baseUrl", url)
                .put("apiKey", key)
                .put("wire", selectedWire)
                .put("model", name)
                .put("policy", approval)
                .put("database", new File(root, "sessions.sqlite").getPath())
                .put("workspace", workspace.getPath())
                .put("appRoot", context.getFilesDir().getParentFile().getCanonicalPath())
                .put("rvmModelPath", rvmModelFile != null ? rvmModelFile.getPath() : "")
                .put("ortRuntimePath", runtime.isFile() ? runtime.getPath() : "")
                .put("caBundle", new File(root, "trusted-roots.pem").getPath()));
    }

    private File prepareBundledModel(String name, String expected) throws Exception {
        File directory = new File(root, "Models");
        if (!directory.mkdirs() && !directory.isDirectory())
            throw new IOException("Cannot create Agent model directory");
        File target = new File(directory, name);
        if (target.isFile() && expected.equals(sha256(target))) return target;
        File staged = new File(directory, name + ".part");
        try {
            try (InputStream input = context.getAssets().open(
                     "MaiAgentModels/" + name);
                 FileOutputStream output = new FileOutputStream(staged)) {
                byte[] bytes = new byte[1024 * 1024];
                int count;
                while ((count = input.read(bytes)) != -1) output.write(bytes, 0, count);
                output.getFD().sync();
            }
            if (!expected.equals(sha256(staged)))
                throw new IOException("Bundled Agent model checksum is invalid: " + name);
            try {
                Files.move(staged.toPath(), target.toPath(),
                    StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE);
            } catch (AtomicMoveNotSupportedException unsupported) {
                Files.move(staged.toPath(), target.toPath(), StandardCopyOption.REPLACE_EXISTING);
            }
            return target;
        } finally {
            Files.deleteIfExists(staged.toPath());
        }
    }

    private static String sha256(File file) throws Exception {
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        try (InputStream input = Files.newInputStream(file.toPath())) {
            byte[] bytes = new byte[1024 * 1024];
            int count;
            while ((count = input.read(bytes)) != -1) digest.update(bytes, 0, count);
        }
        StringBuilder hex = new StringBuilder(64);
        for (byte value : digest.digest()) {
            hex.append(Character.forDigit((value >> 4) & 15, 16));
            hex.append(Character.forDigit(value & 15, 16));
        }
        return hex.toString();
    }
    private void refresh(boolean force) {
        try {
            JSONObject result = invokeNativeAgent(makeOperationRequest("snapshot").put("force", force));
            if (result.optBoolean("changed")) {
                hideReasoningText(result);
                attachMediaMentions(result);
                data = result;
                if (!result.optString("error").isEmpty())
                    error = result.optString("error");
                publishState();
            }
        } catch (Exception e) {
            error = safeMessage(e);
            publishState();
        }
    }
    private static void hideReasoningText(JSONObject snapshot) {
        JSONArray messages = snapshot.optJSONArray("messages");
        if (messages == null) return;
        for (int i = 0; i < messages.length(); i++) {
            JSONObject message = messages.optJSONObject(i);
            JSONArray parts = message == null ? null : message.optJSONArray("parts");
            if (parts == null) continue;
            for (int j = 0; j < parts.length(); j++) {
                JSONObject part = parts.optJSONObject(j);
                if (part != null && part.optString("kind").equals("reasoning"))
                    part.remove("text");
            }
        }
    }
    // 作用：把回复里实际存在的媒体引用关联到消息展示项。
    private void attachMediaMentions(JSONObject snapshot) throws org.json.JSONException {
        JSONArray messages = snapshot.optJSONArray("messages");
        if (messages == null) return;
        if (mediaMentionCache.size() > 500) mediaMentionCache.clear();
        for (int i = 0; i < messages.length(); i++) {
            JSONObject message = messages.optJSONObject(i);
            if (message == null || message.optBoolean("active")
                || (message.optString("role").equals("assistant")
                    && message.optLong("completed") == 0)) continue;
            String key = selected + ":" + message.optString("id") + ":"
                + message.optLong("completed");
            JSONArray cards = mediaMentionCache.get(key);
            if (cards == null) {
                StringBuilder text = new StringBuilder();
                JSONArray parts = message.optJSONArray("parts");
                Set<String> alreadyShown = new HashSet<>();
                if (parts != null)
                    for (int j = 0; j < parts.length(); j++) {
                        JSONObject part = parts.optJSONObject(j);
                        if (part == null) continue;
                        if (part.optString("kind").equals("text"))
                            text.append(part.optString("text")).append('\n');
                        if (part.optString("kind").equals("video")
                            || part.optString("kind").equals("audio")) {
                            File existing = workspaceFile(part.optString("path"));
                            if (existing != null) alreadyShown.add(existing.getPath());
                        }
                        if (part.optString("tool").equals("agent_send_media")
                            && part.optString("state").equals("completed")) {
                            try {
                                JSONObject artifact = new JSONObject(part.optString("output"));
                                File existing = workspaceFile(artifact.optString("path"));
                                if (existing != null) alreadyShown.add(existing.getPath());
                            } catch (org.json.JSONException ignored) { }
                        }
                    }
                List<AIAssistantMediaMentions.Mention> found =
                    AIAssistantMediaMentions.cards(text.toString(), this::workspaceFile);
                cards = new JSONArray();
                for (AIAssistantMediaMentions.Mention mention : found) {
                    if (alreadyShown.contains(mention.file.getPath())) continue;
                    cards.put(new JSONObject()
                        .put("path", mention.file.getAbsolutePath())
                        .put("kind", mention.kind == AIAssistantMediaMentions.Kind.VIDEO
                            ? "video" : "audio"));
                }
                mediaMentionCache.put(key, cards);
            }
            message.put("inline_media", cards);
        }
    }
    private void publishState() {
        JSONObject visible = selectedRemote == null ? data : remoteData;
        String shown = selectedRemote == null ? selected : selectedRemote.optString("id");
        try {
            visible = new JSONObject(visible.toString());
            JSONArray combined = new JSONArray();
            JSONArray local = data.optJSONArray("sessions");
            if (local != null) for (int i = 0; i < local.length(); i++) combined.put(local.get(i));
            for (int i = 0; i < remoteSessions.length(); i++) combined.put(remoteSessions.get(i));
            visible.put("sessions", combined);
        } catch (Exception ignored) { visible = data; }
        String status = pendingRemoteSelection != null
            ? "正在读取 " + pendingRemoteSelection.optString("device_name") + " 的对话…"
            : selectedRemote == null ? "" : remoteCommandStatus;
        State next = new State(visible, selectedRemote, shown, baseUrl, model, policy,
            error, status, ready);
        state = next;
        Listener target = listener;
        if (target != null)
            main.post(() -> {
                if (listener == target && !closed)
                    target.onState(next);
            });
    }

    JSONArray localSessionsForSync() {
        if (listener == null && state.ready && !closed) {
            CompletableFuture<JSONArray> updated = new CompletableFuture<>();
            worker.post(() -> {
                try {
                    JSONObject snapshot = invokeNativeAgent(new JSONObject().put("op", "snapshot")
                        .put("session", "").put("messageLimit", 0).put("force", true));
                    JSONArray sessions = snapshot.optJSONArray("sessions");
                    JSONObject next = new JSONObject(data.toString());
                    next.put("sessions", sessions == null ? new JSONArray() : sessions);
                    data = next;
                    publishState();
                    updated.complete(sessions == null ? new JSONArray() : sessions);
                } catch (Exception failure) {
                    updated.completeExceptionally(failure);
                }
            });
            try { return new JSONArray(updated.get(3, TimeUnit.SECONDS).toString()); }
            catch (Exception ignored) { /* Fall back to the last published Session list. */ }
        }
        JSONArray sessions = data.optJSONArray("sessions");
        if (sessions == null) return new JSONArray();
        try { return new JSONArray(sessions.toString()); }
        catch (Exception ignored) { return new JSONArray(); }
    }

    private static boolean sameRemote(JSONObject first, JSONObject second) {
        return first != null && second != null &&
            first.optString("device_slot").equals(second.optString("device_slot")) &&
            first.optString("login_session_id").equals(second.optString("login_session_id")) &&
            first.optString("id").equals(second.optString("id"));
    }

    void acceptRemoteCatalog(JSONArray entries, String ownSlot) {
        worker.post(() -> {
            JSONArray next = new JSONArray();
            if (entries != null) for (int i = 0; i < entries.length(); i++) {
                JSONObject entry = entries.optJSONObject(i);
                if (entry != null && !ownSlot.equals(entry.optString("device_slot")))
                    next.put(entry);
            }
            remoteSessions = next;
            if (selectedRemote != null) {
                JSONObject updated = null;
                for (int i = 0; i < next.length(); i++) {
                    JSONObject entry = next.optJSONObject(i);
                    if (sameRemote(selectedRemote, entry)) { updated = entry; break; }
                }
                if (updated != null) selectedRemote = updated;
                else try { selectedRemote = new JSONObject(selectedRemote.toString()).put("online", false); }
                     catch (Exception ignored) { /* Keep the prior offline view. */ }
            }
            publishState();
        });
    }

    void markRemoteCatalogOffline() {
        worker.post(() -> {
            boolean changed = false;
            for (int i = 0; i < remoteSessions.length(); i++) {
                JSONObject session = remoteSessions.optJSONObject(i);
                if (session == null || !session.optBoolean("online")) continue;
                try { session.put("online", false); changed = true; }
                catch (Exception ignored) { /* Already decoded JSON. */ }
            }
            if (selectedRemote != null && selectedRemote.optBoolean("online")) {
                try { selectedRemote.put("online", false); changed = true; }
                catch (Exception ignored) { /* Already decoded JSON. */ }
            }
            if (changed) publishState();
        });
    }

    void historyForSync(String sessionId, String before, int limit, Consumer<JSONObject> completion) {
        worker.post(() -> {
            JSONObject result;
            try {
                if (closed) throw new IOException("账号已切换");
                if (!hasLocalSession(sessionId)) throw new IOException("本机会话不存在");
                result = invokeNativeAgent(new JSONObject().put("op", "messages_page")
                    .put("session", sessionId).put("before", before)
                    .put("limit", Math.max(1, Math.min(20, limit))));
                JSONArray messages = result.optJSONArray("messages");
                if (messages != null) for (int i = 0; i < messages.length(); i++) {
                    JSONObject message = messages.optJSONObject(i);
                    if (message == null) continue;
                    JSONArray parts = message.optJSONArray("parts");
                    if (parts == null) continue;
                    for (int j = 0; j < parts.length(); j++) {
                        JSONObject part = parts.optJSONObject(j);
                        if (part != null && (part.optString("kind").equals("image") ||
                            part.optString("kind").equals("video"))) part.remove("path");
                    }
                }
                JSONObject state = invokeNativeAgent(new JSONObject().put("op", "snapshot")
                    .put("session", sessionId).put("messageLimit", 0).put("force", true));
                result.put("permissions", state.optJSONArray("permissions") == null
                    ? new JSONArray() : state.optJSONArray("permissions"));
                result.put("questions", state.optJSONArray("questions") == null
                    ? new JSONArray() : state.optJSONArray("questions"));
                if (listener != null) refresh(true);
            } catch (Exception failure) {
                result = new JSONObject();
                try { result.put("ok", false).put("error", safeMessage(failure)); }
                catch (Exception ignored) { /* JSONObject has no external input here. */ }
            }
            completion.accept(result);
        });
    }

    private boolean hasLocalSession(String sessionId) {
        JSONArray sessions = data.optJSONArray("sessions");
        if (sessions == null) return false;
        for (int i = 0; i < sessions.length(); i++) {
            JSONObject entry = sessions.optJSONObject(i);
            if (entry != null && sessionId.equals(entry.optString("id"))) return true;
        }
        return false;
    }

    void commandForSync(String sessionId, JSONObject action, Consumer<JSONObject> completion) {
        worker.post(() -> {
            JSONObject result = new JSONObject();
            try {
                if (closed) throw new IOException("账号已切换");
                if (!hasLocalSession(sessionId)) throw new IOException("本机会话不存在");
                if (action == null) throw new IOException("远端操作无效");
                String operation = action.optString("action");
                JSONObject request = new JSONObject(action.toString()).put("op", operation)
                    .put("session", sessionId);
                if (operation.equals("prompt")) {
                    String text = action.optString("text");
                    if (text.isEmpty() || text.getBytes(StandardCharsets.UTF_8).length > 16 * 1024)
                        throw new IOException("消息为空或过长");
                    request.put("op", "send").put("images", new JSONArray())
                        .put("videos", new JSONArray()).put("quoteMessageId", "");
                } else if (!operation.equals("stop") && !operation.equals("permission") &&
                    !operation.equals("answer") && !operation.equals("rename")) {
                    throw new IOException("不支持的远端操作");
                }
                invokeNativeAgent(request);
                result.put("ok", true);
                refresh(true);
            } catch (Exception failure) {
                try { result.put("ok", false).put("error", safeMessage(failure)); }
                catch (Exception ignored) { /* JSONObject has no external input here. */ }
            }
            completion.accept(result);
        });
    }

    void acceptRemotePayload(JSONObject envelope, JSONObject payload) {
        worker.post(() -> {
            JSONObject owner = pendingRemoteSelection != null ? pendingRemoteSelection : selectedRemote;
            if (owner == null || !owner.optString("id").equals(envelope.optString("session_id")) ||
                !owner.optString("device_slot").equals(envelope.optString("from_slot")) ||
                !owner.optString("login_session_id").equals(envelope.optString("from_login_session_id")))
                return;
            if (payload.optString("type").equals("command_result")) {
                JSONObject result = payload.optJSONObject("result");
                pendingRemoteCommands = Math.max(0, pendingRemoteCommands - 1);
                if (pendingRemoteCommands == 0) remoteCommandStatus = "";
                if (result != null && !result.optBoolean("ok")) error = result.optString("error");
                else error = "";
                publishState();
                return;
            }
            JSONArray received = payload.optJSONArray("messages");
            if (!payload.optBoolean("ok") || received == null) return;
            for (int i = 0; i < received.length(); i++) {
                JSONObject message = received.optJSONObject(i);
                JSONArray parts = message == null ? null : message.optJSONArray("parts");
                if (parts == null) continue;
                int originalCount = parts.length();
                for (int j = 0; j < originalCount; j++) {
                    JSONObject part = parts.optJSONObject(j);
                    if (part == null) continue;
                    String kind = part.optString("kind");
                    if (!kind.equals("image") && !kind.equals("video")) continue;
                    part.remove("path");
                    try {
                        parts.put(new JSONObject().put("id", part.optString("id") + "-remote-placeholder")
                            .put("kind", "text").put("text", kind.equals("image")
                                ? "[图片在所属设备，暂不可跨设备预览]" :
                                  "[视频在所属设备，暂不可跨设备预览]"));
                    } catch (Exception ignored) { /* Already decoded JSON. */ }
                }
            }
            if (pendingRemoteSelection != null) {
                selectedRemote = pendingRemoteSelection;
                pendingRemoteSelection = null;
                remoteSelectionGeneration++;
                remoteData = payload;
                Map<String, JSONObject> ordered = new java.util.TreeMap<>();
                for (int i = 0; i < received.length(); i++) {
                    JSONObject message = received.optJSONObject(i);
                    if (message != null) ordered.put(message.optString("id"), message);
                }
                JSONArray chronological = new JSONArray();
                for (JSONObject message : ordered.values()) chronological.put(message);
                try { remoteData.put("messages", chronological); }
                catch (Exception ignored) { /* Already decoded JSON. */ }
            } else {
                Map<String, JSONObject> merged = new java.util.TreeMap<>();
                JSONArray current = remoteData.optJSONArray("messages");
                if (current != null) for (int i = 0; i < current.length(); i++) {
                    JSONObject message = current.optJSONObject(i);
                    if (message != null) merged.put(message.optString("id"), message);
                }
                for (int i = 0; i < received.length(); i++) {
                    JSONObject message = received.optJSONObject(i);
                    if (message != null) merged.put(message.optString("id"), message);
                }
                JSONArray combined = new JSONArray();
                for (JSONObject message : merged.values()) combined.put(message);
                try {
                    remoteData.put("messages", combined);
                    if (payload.has("permissions"))
                        remoteData.put("permissions", payload.optJSONArray("permissions"));
                    if (payload.has("questions"))
                        remoteData.put("questions", payload.optJSONArray("questions"));
                } catch (Exception ignored) { /* Already decoded JSON. */ }
            }
            error = "";
            publishState();
        });
    }

    private void noteRemoteCommandAccepted(JSONObject destination) {
        pendingRemoteCommands++;
        remoteCommandStatus = "已送达所属设备，等待确认执行";
        int generation = ++remoteCommandGeneration;
        worker.postDelayed(() -> {
            if (generation != remoteCommandGeneration || pendingRemoteCommands == 0 ||
                !sameRemote(selectedRemote, destination)) return;
            pendingRemoteCommands = 0;
            remoteCommandStatus = "";
            error = "所属设备是否执行了操作尚未确定，请先查看会话记录，不要直接重复发送";
            publishState();
        }, 30000);
    }
    void select(String id) {
        select(id, null);
    }
    void select(String id, Consumer<Boolean> completion) {
        worker.post(() -> {
            boolean success = false;
            try {
                selectedRemote = null;
                pendingRemoteSelection = null;
                remoteSelectionGeneration++;
                pendingRemoteCommands = 0;
                remoteCommandStatus = "";
                selected = id;
                error = "";
                refresh(true);
                success = true;
            } catch (Exception failure) {
                error = safeMessage(failure);
                publishState();
            }
            if (completion != null) {
                boolean result = success;
                main.post(() -> completion.accept(result));
            }
        });
    }
    void selectRemote(JSONObject remote, Consumer<Boolean> completion) {
        worker.post(() -> {
            if (sessionSync == null || !remote.optBoolean("online")) {
                error = "所属设备离线，暂时无法读取这段会话";
                publishState();
                main.post(() -> completion.accept(false));
                return;
            }
            pendingRemoteSelection = remote;
            int generation = ++remoteSelectionGeneration;
            publishState();
            worker.postDelayed(() -> {
                if (generation != remoteSelectionGeneration || pendingRemoteSelection == null) return;
                pendingRemoteSelection = null;
                error = "读取所属设备会话超时，请重新选择";
                publishState();
            }, 20000);
            try {
                JSONObject request = new JSONObject().put("limit", 10);
                sessionSync.sendToRemote(remote, "history_request", request, failure -> {
                    worker.post(() -> {
                        if (!failure.isEmpty() && sameRemote(pendingRemoteSelection, remote)) {
                            pendingRemoteSelection = null;
                            error = failure;
                            publishState();
                        }
                        main.post(() -> completion.accept(failure.isEmpty()));
                    });
                });
            } catch (Exception failure) {
                pendingRemoteSelection = null;
                error = safeMessage(failure);
                publishState();
                main.post(() -> completion.accept(false));
            }
        });
    }
    void create() {
        create(null);
    }
    void create(Consumer<Boolean> completion) {
        actionWithResult("create", new JSONObject(), null, completion);
    }
    void send(String text, Consumer<Boolean> completion) {
        send(text, java.util.Collections.emptyList(), completion);
    }
    void suggestReplies(JSONArray messages, java.util.function.BiConsumer<JSONObject, String> completion) {
        JSONArray context = messages == null ? new JSONArray() : messages;
        worker.post(() -> {
            JSONObject suggestions = null;
            String failure = null;
            try {
                suggestions = invokeNativeAgent(new JSONObject().put("op", "suggest_replies")
                    .put("messages", context));
            } catch (Exception e) {
                failure = safeMessage(e);
            }
            JSONObject result = suggestions;
            String errorMessage = failure;
            main.post(() -> completion.accept(result, errorMessage));
        });
    }
    void send(String text, java.util.List<ImportedFile> attachments, Consumer<Boolean> completion) {
        send(state.selected, text, attachments, "", completion);
    }
    // 发送目标由可见输入栏决定，不再重读可能先于面板更新的控制器选中状态。
    void send(String intendedSession, String text, java.util.List<ImportedFile> attachments,
              Consumer<Boolean> completion) {
        send(intendedSession, text, attachments, "", completion);
    }
    void send(String intendedSession, String text, java.util.List<ImportedFile> attachments,
              String quotedMessageId, Consumer<Boolean> completion) {
        java.util.List<ImportedFile> intendedAttachments = new java.util.ArrayList<>(attachments);
        worker.post(() -> {
            if (selectedRemote != null && intendedSession.equals(selectedRemote.optString("id"))) {
                if (sessionSync == null || !selectedRemote.optBoolean("online") ||
                    !intendedAttachments.isEmpty() || !quotedMessageId.isEmpty() ||
                    text.trim().isEmpty()) {
                    error = "远端设备离线，或消息包含暂不支持的附件/引用";
                    publishState();
                    main.post(() -> completion.accept(false));
                    return;
                }
                JSONObject destination = selectedRemote;
                try {
                    JSONObject request = new JSONObject().put("action", "prompt").put("text", text);
                    sessionSync.sendToRemote(destination, "command", request, failure -> {
                        worker.post(() -> {
                            error = failure;
                            if (failure.isEmpty()) noteRemoteCommandAccepted(destination);
                            publishState();
                            main.post(() -> completion.accept(failure.isEmpty()));
                        });
                    });
                } catch (Exception failure) {
                    error = safeMessage(failure);
                    publishState();
                    main.post(() -> completion.accept(false));
                }
                return;
            }
            boolean success = false;
            try {
                refreshSeedanceCredential(true);
                refreshBailianCredential(true);
                String target = intendedSession;
                if (target.isEmpty()) {
                    target = invokeNativeAgent(makeOperationRequest("create")).getString("id");
                    selected = target;
                }
                JSONArray images = new JSONArray();
                JSONArray videos = new JSONArray();
                for (ImportedFile file : intendedAttachments) if (file.image) {
                    images.put(new JSONObject().put("path", file.relativePath)
                        .put("mimeType", file.mimeType));
                } else if (file.mimeType.startsWith("video/")) {
                    videos.put(new JSONObject().put("path", file.relativePath)
                        .put("mimeType", file.mimeType));
                }
                invokeNativeAgent(makeOperationRequest("send").put("session", target).put("text", text)
                    .put("images", images).put("videos", videos)
                    .put("quoteMessageId", quotedMessageId));
                error = "";
                success = true;
                refresh(true);
            } catch (Exception e) {
                error = safeMessage(e);
                publishState();
            }
            boolean sent = success;
            main.post(() -> completion.accept(sent));
        });
    }
    void switchModel(String name, Consumer<Boolean> completion) {
        save(name, policy, completion);
    }
    void action(String operation, JSONObject values, Runnable completion) {
        actionWithResult(operation, values, completion, null);
    }
    private void actionWithResult(String operation, JSONObject values, Runnable completion,
                                  Consumer<Boolean> settled) {
        String target = values.optString("session", state.selected);
        worker.post(() -> {
            if (selectedRemote != null && !operation.equals("create")) {
                if (sessionSync == null || !selectedRemote.optBoolean("online") ||
                    !(operation.equals("stop") || operation.equals("permission") ||
                      operation.equals("answer") || operation.equals("rename"))) {
                    error = "所属设备离线或不支持此操作";
                    publishState();
                    if (settled != null) main.post(() -> settled.accept(false));
                    return;
                }
                JSONObject destination = selectedRemote;
                try {
                    JSONObject request = new JSONObject(values.toString()).put("action", operation);
                    sessionSync.sendToRemote(destination, "command", request, failure -> {
                        worker.post(() -> {
                            error = failure;
                            if (failure.isEmpty()) noteRemoteCommandAccepted(destination);
                            publishState();
                            if (settled != null) main.post(() -> settled.accept(failure.isEmpty()));
                            if (failure.isEmpty() && completion != null) main.post(completion);
                        });
                    });
                } catch (Exception failure) {
                    error = safeMessage(failure);
                    publishState();
                    if (settled != null) main.post(() -> settled.accept(false));
                }
                return;
            }
            if (operation.equals("create")) selectedRemote = null;
            boolean success = false;
            try {
                JSONObject request =
                    new JSONObject(values.toString()).put("op", operation).put("session", target);
                JSONObject result = invokeNativeAgent(request);
                if (operation.equals("create"))
                    selected = result.getString("id");
                if (operation.equals("delete") && selected.equals(target))
                    selected = "";
                error = "";
                refresh(true);
                success = true;
                if (completion != null)
                    main.post(completion);
            } catch (Exception e) {
                error = safeMessage(e);
                publishState();
            }
            if (settled != null) {
                boolean result = success;
                main.post(() -> settled.accept(result));
            }
        });
    }
    void save(String name, String approval, Consumer<Boolean> completion) {
        worker.post(() -> {
            boolean success = false;
            try {
                String selectedName = name.trim();
                if (!selectedName.equals("glm-5.3") && !selectedName.equals("glm-5.3-flash") &&
                    !selectedName.equals("deepseek-flash"))
                    throw new IllegalArgumentException("请选择受支持的主模型");
                String effectiveUrl = selectedName.equals("deepseek-flash")
                    ? DEEPSEEK_URL : GLM_RESPONSES_URL;
                if (testEndpoint == null && (glmApiKey.isEmpty() || deepseekApiKey.isEmpty()))
                    refreshAccountModelKeys();
                String key = selectedName.equals("deepseek-flash") ? deepseekApiKey : glmApiKey;
                if (testEndpoint == null && key.isEmpty())
                    throw new IOException("请先登录已开通模型服务的账号");
                configure(effectiveUrl, selectedName, approval, key, "responses");
                byte[] config = new JSONObject().put("baseUrl", effectiveUrl)
                    .put("model", selectedName).put("policy", approval)
                    .put("wire", "responses")
                    .toString().getBytes(StandardCharsets.UTF_8);
                android.util.AtomicFile file =
                    new android.util.AtomicFile(new File(root, "settings.json"));
                FileOutputStream out = file.startWrite();
                try {
                    out.write(config);
                    file.finishWrite(out);
                } catch (Exception failure) {
                    file.failWrite(out);
                    configure(baseUrl, model, policy, apiKey, "responses");
                    throw failure;
                }
                baseUrl = effectiveUrl;
                model = selectedName;
                policy = approval;
                apiKey = key;
                wire = "responses";
                error = "";
                success = true;
                refresh(true);
            } catch (Exception failure) {
                error = safeMessage(failure);
                publishState();
            }
            boolean saved = success;
            main.post(() -> completion.accept(saved));
        });
    }
    static String endpointFor(String name, String selectedWire, String url, String chatUrl) {
        if (name.equals("deepseek-flash")) return DEEPSEEK_URL;
        if (!name.startsWith("glm-") || !url.startsWith("https://open.bigmodel.cn/"))
            return url;
        if (selectedWire.equals("responses")) return GLM_RESPONSES_URL;
        return url.equals(GLM_RESPONSES_URL) ? chatUrl : url;
    }
    private void refreshAccountModelKeys() throws Exception {
        MaiChatAuthClient.AccountModelKeys keys = accountAuth.accountModelKeys();
        if (keys == null) return;
        glmApiKey = keys.glm;
        deepseekApiKey = keys.deepseek;
        miniMaxApiKey = keys.minimax;
        apiKey = model.equals("deepseek-flash") ? deepseekApiKey : glmApiKey;
    }

    private void refreshSeedanceCredential(boolean force) {
        if (testEndpoint != null) return;
        long now = System.currentTimeMillis() / 1000L;
        if (arkExpiresAt > now + 90 || (!force && now < nextArkRefreshAt)) return;
        nextArkRefreshAt = now + 30;
        if (arkExpiresAt <= now) arkApiKey = "";
        try {
            MaiChatAuthClient.ModelCredential issued = accountAuth.seedanceCredential();
            if (issued == null) return;
            arkApiKey = issued.apiKey;
            arkExpiresAt = issued.expiresAt;
        } catch (Exception failure) {
            Log.w("MaiChatAgent", "Seedance temporary credential renewal failed");
        }
    }
    private void refreshBailianCredential(boolean force) {
        // 手机号会话续领百炼短期 Key；万相和百炼可灵共用，过期后不继续暴露旧值。
        if (testEndpoint != null) return;
        long now = System.currentTimeMillis() / 1000L;
        if (wanExpiresAt > now + 90 || (!force && now < nextWanRefreshAt)) return;
        nextWanRefreshAt = now + 30;
        if (wanExpiresAt <= now) {
            wanApiKey = "";
            wanWorkspaceId = "";
        }
        try {
            MaiChatAuthClient.BailianCredential issued = accountAuth.bailianCredential();
            if (issued == null) return;
            wanWorkspaceId = issued.workspaceId;
            wanApiKey = issued.apiKey;
            wanExpiresAt = issued.expiresAt;
        } catch (Exception failure) {
            Log.w("MaiChatAgent", "Bailian temporary credential renewal failed");
        }
    }
    void importFile(Uri uri, Consumer<ImportedFile> completion) {
        worker.post(() -> {
            ImportedFile imported = null;
            try (InputStream input = context.getContentResolver().openInputStream(uri)) {
                if (input == null)
                    throw new IllegalArgumentException("无法读取文件");
                String mime = context.getContentResolver().getType(uri);
                imported = importStream(input, mime == null ? "application/octet-stream" : mime,
                    uri.getLastPathSegment());
            } catch (Exception e) {
                error = safeMessage(e);
                publishState();
            }
            ImportedFile result = imported;
            main.post(() -> completion.accept(result));
        });
    }

    // 相册输入在工作线程导入；图片转换为模型支持的格式，视频保留原件供 Agent 读取。
    void importComposerMedia(Uri uri, Consumer<ImportedFile> completion) {
        worker.post(() -> {
            ImportedFile imported = null;
            try {
                String mime = context.getContentResolver().getType(uri);
                if (mime == null) throw new IllegalArgumentException("无法识别媒体格式");
                mime = mime.toLowerCase(java.util.Locale.ROOT);
                if (mime.startsWith("video/")) {
                    imported = importOriginalMediaForHost(uri);
                } else if (mime.startsWith("image/")) {
                    try (InputStream input = context.getContentResolver().openInputStream(uri)) {
                        if (input == null) throw new IOException("无法读取图片");
                        imported = importStream(input, mime, uri.getLastPathSegment());
                    }
                } else {
                    throw new IllegalArgumentException("只能添加图片或视频");
                }
                imported = new ImportedFile(imported.relativePath, imported.mimeType,
                    imported.image, sourceDisplayName(uri, imported.relativePath));
            } catch (Exception e) {
                error = safeMessage(e);
                publishState();
            }
            ImportedFile result = imported;
            main.post(() -> completion.accept(result));
        });
    }
    private String sourceDisplayName(Uri uri, String fallback) {
        try (Cursor cursor = context.getContentResolver().query(
                 uri, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) {
                String value = cursor.getString(0);
                if (value != null && !value.trim().isEmpty()) return value.trim();
            }
        } catch (Exception ignored) {
            // 部分媒体 Provider 不提供显示名；使用工作区文件名作为回退。
        }
        return fallback;
    }

    public ImportedFile importPhotoForHost(Uri uri) throws Exception {
        if (root == null) throw new IllegalStateException("AI 工作区尚未准备好");
        if (android.os.Build.VERSION.SDK_INT >= 29) {
            Bitmap bitmap = context.getContentResolver().loadThumbnail(
                uri, new android.util.Size(2048, 2048), null);
            if (bitmap == null) throw new IllegalArgumentException("无法读取照片");
            java.io.ByteArrayOutputStream jpeg = new java.io.ByteArrayOutputStream();
            try {
                if (!bitmap.compress(Bitmap.CompressFormat.JPEG, 88, jpeg))
                    throw new IllegalArgumentException("无法转换照片");
            } finally {
                bitmap.recycle();
            }
            try (InputStream input = new java.io.ByteArrayInputStream(jpeg.toByteArray())) {
                return importStream(input, "image/jpeg", "photo.jpg");
            }
        }
        try (InputStream input = context.getContentResolver().openInputStream(uri)) {
            if (input == null) throw new IllegalArgumentException("无法读取照片");
            return importStream(input, "image/jpeg", "photo.jpg");
        }
    }
    public ImportedFile importOriginalMediaForHost(Uri uri) throws Exception {
        if (root == null) throw new IllegalStateException("AI 工作区尚未准备好");
        String mime = context.getContentResolver().getType(uri);
        if (mime != null) mime = mime.toLowerCase(java.util.Locale.ROOT);
        if (mime == null || !(mime.startsWith("image/") || mime.startsWith("video/")))
            throw new IllegalArgumentException("只能导入系统图库中的图片或视频");
        boolean image = mime.startsWith("image/");
        String extension;
        switch (mime.toLowerCase(java.util.Locale.ROOT)) {
        case "image/jpeg": extension = "jpg"; break;
        case "image/png": extension = "png"; break;
        case "image/webp": extension = "webp"; break;
        case "image/gif": extension = "gif"; break;
        case "image/heic":
        case "image/heif": extension = "heic"; break;
        case "video/mp4": extension = "mp4"; break;
        case "video/quicktime": extension = "mov"; break;
        case "video/x-matroska": extension = "mkv"; break;
        case "video/3gpp": extension = "3gp"; break;
        case "video/webm": extension = "webm"; break;
        default: throw new IllegalArgumentException("不支持的系统媒体格式：" + mime);
        }
        String name = "gallery-" + UUID.randomUUID() + "." + extension;
        File target = new File(new File(root, "Workspace"), name);
        try (InputStream input = context.getContentResolver().openInputStream(uri);
             FileOutputStream output = new FileOutputStream(target)) {
            if (input == null) throw new IOException("无法读取系统媒体");
            byte[] buffer = new byte[8192];
            long copied = 0;
            int count;
            while ((count = input.read(buffer)) >= 0) {
                copied += count;
                if (copied > 100L * 1024 * 1024)
                    throw new IllegalArgumentException(image ? "照片超过 100 MB" : "视频超过 100 MB");
                output.write(buffer, 0, count);
            }
            if (copied == 0) throw new IOException("媒体文件为空");
        } catch (Exception error) {
            target.delete();
            throw error;
        }
        return new ImportedFile(name, mime, image);
    }
    void importCameraFile(File source, Consumer<ImportedFile> completion) {
        worker.post(() -> {
            ImportedFile imported = null;
            try (InputStream input = new java.io.FileInputStream(source)) {
                imported = importStream(input, "image/jpeg", source.getName());
            } catch (Exception e) {
                error = safeMessage(e);
                publishState();
            } finally {
                if (source != null) source.delete();
            }
            if (imported != null)
                imported = new ImportedFile(imported.relativePath, imported.mimeType, true, "拍摄照片");
            ImportedFile result = imported;
            main.post(() -> completion.accept(result));
        });
    }
    void prepareCameraFile(Consumer<File> completion) {
        worker.post(() -> {
            File value = null;
            try {
                File directory = new File(context.getCacheDir(), "AIAssistantCaptures");
                if (!directory.mkdirs() && !directory.isDirectory())
                    throw new IllegalStateException("无法创建拍照目录");
                value = File.createTempFile("capture-", ".jpg", directory);
            } catch (Exception e) {
                error = safeMessage(e);
                publishState();
            }
            File result = value;
            main.post(() -> completion.accept(result));
        });
    }
    public File workspaceFile(String relativePath) {
        if (root == null || relativePath == null || relativePath.trim().isEmpty()) return null;
        try { return resolveAppFile(relativePath); }
        catch (IOException | IllegalArgumentException error) { return null; }
    }
    void discardComposerMedia(ImportedFile file) {
        if (file == null) return;
        worker.post(() -> {
            String path = file.relativePath;
            if (root == null || path == null || path.contains("/") || path.contains("\\")
                || !(path.startsWith("image-") || path.startsWith("gallery-")
                    || path.startsWith("import-"))) return;
            File workspace = new File(root, "Workspace");
            File candidate = new File(workspace, path);
            try {
                if (candidate.getCanonicalFile().getParentFile().equals(workspace.getCanonicalFile()))
                    Files.deleteIfExists(candidate.toPath());
            } catch (IOException failure) {
                Log.w("MaiChatAgent", "Could not discard unused media", failure);
            }
        });
    }
    private File resolveAppFile(String path) throws IOException {
        if (root == null)
            throw new IllegalArgumentException("需要 App 目录中的文件路径");
        return AIAssistantPathPolicy.resolve(new File(root, "Workspace"),
            context.getFilesDir().getParentFile(), path);
    }
    public File workspaceImageForHost(String relativePath) throws Exception {
        File candidate = resolveAppFile(relativePath);
        if (!candidate.isFile())
            throw new IllegalArgumentException("图片必须位于当前 App 目录内");
        return candidate;
    }
    // 作用：校验工作区图片并执行宿主图片转换。
    public ImportedFile transformImageForHost(JSONObject arguments) throws Exception {
        File source = workspaceImageForHost(arguments.optString("path", ""));
        if (source.length() < 1 || source.length() > 50L * 1024 * 1024)
            throw new IllegalArgumentException("源图片不能超过 50 MB");
        BitmapFactory.Options bounds = new BitmapFactory.Options();
        bounds.inJustDecodeBounds = true;
        BitmapFactory.decodeFile(source.getPath(), bounds);
        if (bounds.outWidth < 1 || bounds.outHeight < 1
                || (long) bounds.outWidth * bounds.outHeight > 12_000_000)
            throw new IllegalArgumentException("图片不能超过 1200 万像素");
        BitmapFactory.Options decode = new BitmapFactory.Options();
        decode.inPreferredConfig = Bitmap.Config.ARGB_8888;
        Bitmap decoded = BitmapFactory.decodeFile(source.getPath(), decode);
        if (decoded == null) throw new IllegalArgumentException("无法解码源图片");
        Bitmap upright = decoded;
        Bitmap edited = null;
        File target = null;
        try {
            Matrix orientation = new Matrix();
            try {
                int exif = new ExifInterface(source.getPath()).getAttributeInt(
                    ExifInterface.TAG_ORIENTATION, ExifInterface.ORIENTATION_NORMAL);
                switch (exif) {
                case ExifInterface.ORIENTATION_FLIP_HORIZONTAL: orientation.setScale(-1, 1); break;
                case ExifInterface.ORIENTATION_ROTATE_180: orientation.setRotate(180); break;
                case ExifInterface.ORIENTATION_FLIP_VERTICAL: orientation.setScale(1, -1); break;
                case ExifInterface.ORIENTATION_TRANSPOSE:
                    orientation.setRotate(90); orientation.postScale(-1, 1); break;
                case ExifInterface.ORIENTATION_ROTATE_90: orientation.setRotate(90); break;
                case ExifInterface.ORIENTATION_TRANSVERSE:
                    orientation.setRotate(270); orientation.postScale(-1, 1); break;
                case ExifInterface.ORIENTATION_ROTATE_270: orientation.setRotate(270); break;
                default: break;
                }
            } catch (IOException ignored) { /* Images without EXIF are already upright. */ }
            if (!orientation.isIdentity())
                upright = Bitmap.createBitmap(decoded, 0, 0, decoded.getWidth(), decoded.getHeight(),
                    orientation, true);
            int[] dimensions = new int[2];
            byte[] pixels = nativeFilterBitmap(upright, arguments.toString(), dimensions);
            if (pixels == null || dimensions[0] < 1 || dimensions[1] < 1)
                throw new IllegalStateException("图片处理没有返回结果");
            edited = Bitmap.createBitmap(dimensions[0], dimensions[1], Bitmap.Config.ARGB_8888);
            edited.copyPixelsFromBuffer(ByteBuffer.wrap(pixels));
            String name = "edited-" + UUID.randomUUID() + ".png";
            target = new File(new File(root, "Workspace"), name);
            try (FileOutputStream output = new FileOutputStream(target)) {
                if (!edited.compress(Bitmap.CompressFormat.PNG, 100, output))
                    throw new IOException("无法写入处理结果");
            }
            if (target.length() < 1 || target.length() > 50L * 1024 * 1024)
                throw new IllegalArgumentException("处理结果超过 50 MB");
            return new ImportedFile(name, "image/png", true);
        } catch (Exception error) {
            if (target != null) target.delete();
            throw error;
        } finally {
            if (edited != null) edited.recycle();
            if (upright != decoded) upright.recycle();
            decoded.recycle();
        }
    }
    File workspaceDirectoryForPreview() {
        return root == null ? null : context.getFilesDir().getParentFile();
    }
    public File workspacePdfOutputForHost(String absolutePath) throws Exception {
        File candidate = resolveAppFile(absolutePath);
        if (!candidate.getName().toLowerCase(java.util.Locale.ROOT).endsWith(".pdf")
            || candidate.exists())
            throw new IllegalArgumentException("PDF 必须是当前 App 目录内的新文件");
        return candidate;
    }
    public File workspaceDiagnosticOutputForHost() throws Exception {
        if (root == null) throw new IllegalStateException("AI 工作区尚未准备好");
        File workspace = new File(root, "Workspace");
        if (!workspace.isDirectory() && !workspace.mkdirs())
            throw new IOException("无法创建 AI 工作区");
        return new File(workspace, "maichat-diagnostics-"
            + UUID.randomUUID().toString().toLowerCase(java.util.Locale.ROOT) + ".txt");
    }
    // 将选中的外部输入流导入 Agent 工作区，并依据实际媒体类型生成附件元数据。
    // 拷贝失败时清理临时内容，不能把只写入一部分的文件交给消息界面。
    private ImportedFile importStream(InputStream input, String sourceMime, String sourceName)
        throws Exception {
        String originalMime = sourceMime == null ? "application/octet-stream"
            : sourceMime.toLowerCase(java.util.Locale.ROOT);
        boolean image = originalMime.startsWith("image/")
            || AIAssistantMediaPolicy.looksLikeImageName(sourceName);
        boolean unknownType = originalMime.equals("application/octet-stream");
        int maximum = AIAssistantMediaPolicy.maximumBytes(image || unknownType);
        java.io.ByteArrayOutputStream bytes = new java.io.ByteArrayOutputStream();
        byte[] buffer = new byte[8192];
        int count;
        while ((count = input.read(buffer)) >= 0) {
            if (bytes.size() + count > maximum)
                throw new IllegalArgumentException(image
                    ? "请选择不超过 20 MB 的图片" : "请选择不超过 5 MB 的文本文件");
            bytes.write(buffer, 0, count);
        }
        byte[] value = bytes.toByteArray();
        String mime = originalMime;
        if (!image && unknownType) {
            BitmapFactory.Options probe = new BitmapFactory.Options();
            probe.inJustDecodeBounds = true;
            BitmapFactory.decodeByteArray(value, 0, value.length, probe);
            image = probe.outWidth > 0 && probe.outHeight > 0;
        }
        if (image) {
            BitmapFactory.Options bounds = new BitmapFactory.Options();
            bounds.inJustDecodeBounds = true;
            BitmapFactory.decodeByteArray(value, 0, value.length, bounds);
            if (bounds.outWidth <= 0 || bounds.outHeight <= 0)
                throw new IllegalArgumentException("图片格式无法读取");
            if (!AIAssistantMediaPolicy.isSupportedImageMime(mime)) {
                BitmapFactory.Options decode = new BitmapFactory.Options();
                decode.inSampleSize = MessageImageDecodePolicy.sampleSize(
                    bounds.outWidth, bounds.outHeight, 4096, 4096);
                Bitmap bitmap = BitmapFactory.decodeByteArray(value, 0, value.length, decode);
                if (bitmap == null) throw new IllegalArgumentException("图片格式无法转换");
                java.io.ByteArrayOutputStream converted = new java.io.ByteArrayOutputStream();
                try {
                    if (!bitmap.compress(Bitmap.CompressFormat.JPEG, 92, converted))
                        throw new IllegalArgumentException("图片格式无法转换");
                } finally {
                    bitmap.recycle();
                }
                value = converted.toByteArray();
                mime = "image/jpeg";
                if (value.length > AIAssistantMediaPolicy.maximumBytes(true))
                    throw new IllegalArgumentException("转换后的图片超过 20 MB");
            }
        } else {
            if (value.length > AIAssistantMediaPolicy.maximumBytes(false))
                throw new IllegalArgumentException("请选择不超过 5 MB 的文本文件");
            StandardCharsets.UTF_8.newDecoder().decode(java.nio.ByteBuffer.wrap(value));
            mime = "text/plain";
        }
        String extension = image ? AIAssistantMediaPolicy.extension(mime) : "txt";
        String name = (image ? "image-" : "import-")
            + UUID.randomUUID().toString().substring(0, 8) + "." + extension;
        File target = new File(new File(root, "Workspace"), name);
        try {
            Files.write(target.toPath(), value);
        } catch (IOException failure) {
            target.delete();
            throw failure;
        }
        return new ImportedFile(name, mime, image);
    }
    private void clearLegacyModelKeyFiles() {
        for (String name : new String[] {"api-key.enc", "ark-api-key.enc",
                "glm-main-api-key.enc", "glm-video-api-key.enc",
                "deepseek-main-api-key.enc", "wan-api-key.enc",
                "kling-api-key.enc", "minimax-api-key.enc",
                "cloud-service-token.enc", "cloud-service-url.txt"}) {
            new File(root, name).delete();
            new File(root, name + ".bak").delete();
            new File(root, name + ".new").delete();
        }
        try {
            KeyStore store = KeyStore.getInstance("AndroidKeyStore");
            store.load(null);
            if (store.containsAlias("MaiChat.AIAssistant.APIKey"))
                store.deleteEntry("MaiChat.AIAssistant.APIKey");
        } catch (Exception failure) {
            Log.w("MaiChatAgent", "Stored service token cleanup failed", failure);
        }
    }
    private void exportSystemCertificates() throws Exception {
        SystemCertificateBundle.writeCurrentPem(new File(root, "trusted-roots.pem"));
    }
    void close() {
        closed = true;
        if (sessionSync != null) sessionSync.close();
        listener = null;
        worker.post(() -> {
            worker.removeCallbacks(poll);
            if (handle != 0)
                nativeDestroy(handle);
            handle = 0;
            thread.quitSafely();
        });
    }
}
