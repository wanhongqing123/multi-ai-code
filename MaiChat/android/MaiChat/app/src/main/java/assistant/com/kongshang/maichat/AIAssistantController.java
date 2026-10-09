package com.kongshang.maichat;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Matrix;
import android.media.ExifInterface;
import android.net.Uri;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;
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
import java.security.cert.X509Certificate;
import java.util.UUID;
import java.util.concurrent.CountDownLatch;
import java.util.function.Consumer;
import javax.crypto.Cipher;
import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;
import javax.crypto.spec.GCMParameterSpec;
import javax.net.ssl.TrustManager;
import javax.net.ssl.TrustManagerFactory;
import javax.net.ssl.X509TrustManager;
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
        final String selected, baseUrl, model, policy, error;
        final boolean ready;
        State(JSONObject data, String selected, String baseUrl, String model, String policy, String error,
            boolean ready) {
            this.data = data;
            this.selected = selected;
            this.baseUrl = baseUrl;
            this.model = model;
            this.policy = policy;
            this.error = error;
            this.ready = ready;
        }
        boolean busy() {
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
        final boolean image;
        ImportedFile(String relativePath, String mimeType, boolean image) {
            this.relativePath = relativePath;
            this.mimeType = mimeType;
            this.image = image;
        }
    }
    private static AIAssistantController shared;
    public static synchronized AIAssistantController shared(Context context) {
        if (shared == null)
            shared = new AIAssistantController(context, null);
        return shared;
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
    private volatile Listener listener;
    private volatile HostToolHandler hostToolHandler;
    static final String GLM_CHAT_URL = "https://open.bigmodel.cn/api/coding/paas/v4";
    static final String GLM_RESPONSES_URL = "https://open.bigmodel.cn/api/v1";
    static final String DEEPSEEK_URL = "https://api.deepseek.com";
    volatile State state = new State(new JSONObject(), "", GLM_RESPONSES_URL,
        "glm-5.3", "on-request", "", false);
    private long handle;
    private File root;
    private File rvmModelFile;
    private volatile String selected = "", baseUrl = state.baseUrl, model = state.model, policy = state.policy,
                   error = "", apiKey = "", wire = "responses";
    private volatile String deepseekApiKey = "";
    private volatile String glmApiKey = "";
    private volatile String glmVideoApiKey = "";
    private volatile String arkApiKey = "";
    private volatile String wanApiKey = "";
    private volatile String wanWorkspaceId = "";
    private volatile String klingApiKey = "";
    private volatile String miniMaxApiKey = "";
    private volatile String cloudServiceUrl = "";
    private JSONObject data = new JSONObject();
    private boolean ready, closed;
    private final Runnable poll = new Runnable() {
        @Override
        public void run() {
            if (closed || listener == null)
                return;
            if (ready)
                refresh(false);
            worker.postDelayed(this, data.optBoolean("busy") ? 100 : 500);
        }
    };

    AIAssistantController(Context context, String testEndpoint) {
        this.context = context.getApplicationContext();
        if (testEndpoint != null && !BuildConfig.DEBUG)
            throw new IllegalArgumentException("Test endpoint in release build");
        this.testEndpoint = testEndpoint;
        thread.start();
        worker = new Handler(thread.getLooper());
        worker.post(this::initialize);
    }
    void setListener(Listener next) {
        listener = next;
        worker.post(() -> {
            worker.removeCallbacks(poll);
            if (next != null && !closed) {
                emit();
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
                    .put("output", new JSONObject().put("key", arkApiKey))
                    .toString().getBytes(StandardCharsets.UTF_8);
            } catch (Exception failure) {
                return hostToolFailure("Ark key is unavailable", "not_configured");
            }
        }
        if (tool.equals("mobile_ark_service_config")) {
            try {
                String token = readEncryptedKey("cloud-service-token.enc");
                java.net.URI source = new java.net.URI(cloudServiceUrl);
                if (!"https".equals(source.getScheme()) || source.getHost() == null ||
                    source.getRawUserInfo() != null || source.getRawQuery() != null ||
                    source.getRawFragment() != null || token.length() < 32)
                    return hostToolFailure("Ark Assets service is not configured", "not_configured");
                String path = source.getPath() == null ? "" : source.getPath();
                if (path.endsWith("/sign-upload"))
                    path = path.substring(0, path.length() - "/sign-upload".length());
                while (path.endsWith("/")) path = path.substring(0, path.length() - 1);
                java.net.URI base = new java.net.URI("https", null, source.getHost(),
                    source.getPort(), path, null, null);
                return new JSONObject().put("ok", true)
                    .put("output", new JSONObject().put("base_url", base.toString())
                        .put("token", token))
                    .toString().getBytes(StandardCharsets.UTF_8);
            } catch (Exception failure) {
                return hostToolFailure("Ark Assets service is not configured", "not_configured");
            }
        }
        if (tool.equals("glm_api_key")) {
            try {
                return new JSONObject().put("ok", true)
                    .put("output", new JSONObject().put("key",
                        glmVideoApiKey.isEmpty() ? glmApiKey : glmVideoApiKey))
                    .toString().getBytes(StandardCharsets.UTF_8);
            } catch (Exception failure) {
                return hostToolFailure("GLM key is unavailable", "not_configured");
            }
        }
        if (tool.equals("wan_credentials")) {
            try {
                return new JSONObject().put("ok", true)
                    .put("output", new JSONObject().put("key", wanApiKey)
                        .put("workspace_id", wanWorkspaceId))
                    .toString().getBytes(StandardCharsets.UTF_8);
            } catch (Exception failure) {
                return hostToolFailure("Wan credentials are unavailable", "not_configured");
            }
        }
        if (tool.equals("kling_api_key") || tool.equals("minimax_api_key")) {
            try {
                String key = tool.equals("kling_api_key") ? klingApiKey : miniMaxApiKey;
                return new JSONObject().put("ok", true)
                    .put("output", new JSONObject().put("key", key))
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
    private void initialize() {
        try {
            System.loadLibrary("maichat_agent");
            handle = nativeCreate();
            if (!nativeSetHostToolHandler(handle, this))
                throw new IllegalStateException("无法注册 MaiChat 宿主工具");
            root = new File(context.getNoBackupFilesDir(),
                testEndpoint == null ? "AIAssistant" : "AIAssistantTest-" + UUID.randomUUID());
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
                wanWorkspaceId = saved.optString("wanWorkspaceId", "");
            }
            if (!model.equals("glm-5.3") && !model.equals("glm-5.3-flash") &&
                !model.equals("deepseek-flash")) model = "glm-5.3";
            wire = "responses";
            if (model.equals("deepseek-flash")) baseUrl = DEEPSEEK_URL;
            else baseUrl = GLM_RESPONSES_URL;
            File cloudUrlFile = new File(root, "cloud-service-url.txt");
            if (cloudUrlFile.isFile())
                cloudServiceUrl = new String(Files.readAllBytes(cloudUrlFile.toPath()),
                    StandardCharsets.UTF_8).trim();
            if (testEndpoint == null && !cloudServiceUrl.isEmpty()) {
                try {
                    String token = readEncryptedKey("cloud-service-token.enc");
                    if (!token.isEmpty()) applyCloudCredentials(fetchCloudCredentials(cloudServiceUrl, token));
                } catch (Exception failure) {
                    Log.w("MaiChatAgent", "Cloud credential synchronization is unavailable");
                }
            }
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
            emit();
        }
    }
    private static String safeMessage(Throwable error) {
        String detail = error.getMessage();
        return detail == null || detail.trim().isEmpty() ? "AI 助手操作失败" : detail;
    }
    private JSONObject call(JSONObject request) throws Exception {
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
    private JSONObject op(String name) throws Exception {
        return new JSONObject().put("op", name).put("session", selected);
    }
    private void configure(String url, String name, String approval, String key,
                           String selectedWire) throws Exception {
        File workspace = new File(root, "Workspace");
        if (!workspace.mkdirs() && !workspace.isDirectory())
            throw new IllegalStateException("无法创建工作区");
        File runtime = new File(context.getApplicationInfo().nativeLibraryDir,
            "libonnxruntime.so");
        call(op("configure")
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
            JSONObject result = call(op("snapshot").put("force", force));
            if (result.optBoolean("changed")) {
                data = result;
                if (!result.optString("error").isEmpty())
                    error = result.optString("error");
                emit();
            }
        } catch (Exception e) {
            error = safeMessage(e);
            emit();
        }
    }
    private void emit() {
        State next = new State(data, selected, baseUrl, model, policy, error, ready);
        state = next;
        Listener target = listener;
        if (target != null)
            main.post(() -> {
                if (listener == target && !closed)
                    target.onState(next);
            });
    }
    void select(String id) {
        worker.post(() -> {
            selected = id;
            error = "";
            refresh(true);
        });
    }
    void create() {
        action("create", new JSONObject(), null);
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
                suggestions = call(new JSONObject().put("op", "suggest_replies")
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
        String intendedSession = state.selected;
        java.util.List<ImportedFile> intendedAttachments = new java.util.ArrayList<>(attachments);
        worker.post(() -> {
            boolean success = false;
            try {
                String target = intendedSession;
                if (target.isEmpty()) {
                    target = call(op("create")).getString("id");
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
                call(op("send").put("session", target).put("text", text)
                    .put("images", images).put("videos", videos));
                error = "";
                success = true;
                refresh(true);
            } catch (Exception e) {
                error = safeMessage(e);
                emit();
            }
            boolean sent = success;
            main.post(() -> completion.accept(sent));
        });
    }
    void switchModel(String name, Consumer<Boolean> completion) {
        save(name, policy, completion);
    }
    void action(String operation, JSONObject values, Runnable completion) {
        String target = state.selected;
        worker.post(() -> {
            try {
                JSONObject request =
                    new JSONObject(values.toString()).put("op", operation).put("session", target);
                JSONObject result = call(request);
                if (operation.equals("create"))
                    selected = result.getString("id");
                if (operation.equals("delete") && selected.equals(target))
                    selected = "";
                error = "";
                refresh(true);
                if (completion != null)
                    main.post(completion);
            } catch (Exception e) {
                error = safeMessage(e);
                emit();
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
                String key = selectedName.equals("deepseek-flash") ? deepseekApiKey : glmApiKey;
                configure(effectiveUrl, selectedName, approval, key, "responses");
                byte[] config = new JSONObject().put("baseUrl", effectiveUrl)
                    .put("model", selectedName).put("policy", approval)
                    .put("wire", "responses").put("wanWorkspaceId", wanWorkspaceId)
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
                emit();
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
    String wanWorkspaceId() { return wanWorkspaceId; }
    String cloudServiceUrl() { return cloudServiceUrl; }
    void saveCloudService(String address, String newToken, Consumer<Boolean> completion) {
        worker.post(() -> {
            boolean success = false;
            try {
                String nextUrl = address.trim();
                if (nextUrl.isEmpty() && newToken.trim().isEmpty()) {
                    success = true;
                } else {
                    String token = newToken.trim().isEmpty()
                        ? readEncryptedKey("cloud-service-token.enc") : newToken.trim();
                    JSONObject response = fetchCloudCredentials(nextUrl, token);
                    applyCloudCredentials(response);
                    if (!newToken.trim().isEmpty())
                        writeEncryptedKey("cloud-service-token.enc", token);
                    android.util.AtomicFile file =
                        new android.util.AtomicFile(new File(root, "cloud-service-url.txt"));
                    FileOutputStream out = file.startWrite();
                    try {
                        out.write(nextUrl.getBytes(StandardCharsets.UTF_8));
                        file.finishWrite(out);
                    } catch (Exception failure) {
                        file.failWrite(out);
                        throw failure;
                    }
                    cloudServiceUrl = nextUrl;
                    success = true;
                }
                error = "";
            } catch (Exception failure) {
                error = safeMessage(failure);
                emit();
            }
            boolean result = success;
            main.post(() -> completion.accept(result));
        });
    }
    private JSONObject fetchCloudCredentials(String address, String token) throws Exception {
        java.net.URI source = new java.net.URI(address);
        if (!"https".equals(source.getScheme()) || source.getHost() == null ||
            source.getRawUserInfo() != null || source.getRawQuery() != null ||
            source.getRawFragment() != null || token.length() < 32 ||
            token.contains("\r") || token.contains("\n"))
            throw new IllegalArgumentException("请填写有效的云端服务 HTTPS 地址和令牌");
        String basePath = source.getPath() == null ? "" : source.getPath();
        if (basePath.endsWith("/sign-upload"))
            basePath = basePath.substring(0, basePath.length() - "sign-upload".length());
        else if (!basePath.endsWith("/")) basePath += "/";
        java.net.URI endpoint = new java.net.URI("https", null, source.getHost(), source.getPort(),
            basePath + "credentials", null, null);
        javax.net.ssl.HttpsURLConnection connection =
            (javax.net.ssl.HttpsURLConnection) endpoint.toURL().openConnection();
        connection.setRequestMethod("POST");
        connection.setConnectTimeout(10_000);
        connection.setReadTimeout(20_000);
        connection.setUseCaches(false);
        connection.setInstanceFollowRedirects(false);
        connection.setDoOutput(true);
        connection.setRequestProperty("Authorization", "Bearer " + token);
        connection.setRequestProperty("Content-Type", "application/json");
        byte[] body = new JSONObject().put("action", "fetch")
            .put("providers", new JSONArray()
                .put("ark").put("glm").put("glm_video").put("deepseek")
                .put("wan").put("kling").put("minimax"))
            .toString().getBytes(StandardCharsets.UTF_8);
        try {
            try (java.io.OutputStream output = connection.getOutputStream()) { output.write(body); }
            int status = connection.getResponseCode();
            if (status != 200)
                throw new IOException(status == 401 ? "云端服务令牌不匹配" : "云端密钥服务暂时不可用");
            try (InputStream input = connection.getInputStream();
                 java.io.ByteArrayOutputStream bytes = new java.io.ByteArrayOutputStream()) {
                byte[] buffer = new byte[2048];
                int count;
                while ((count = input.read(buffer)) >= 0) {
                    if (bytes.size() + count > 16_384)
                        throw new IOException("云端密钥响应过大");
                    bytes.write(buffer, 0, count);
                }
                return new JSONObject(bytes.toString(StandardCharsets.UTF_8.name()));
            }
        } finally {
            connection.disconnect();
        }
    }
    private void applyCloudCredentials(JSONObject response) throws Exception {
        JSONObject keys = response.optJSONObject("api_keys");
        if (keys == null) throw new IOException("云端密钥响应无效");
        arkApiKey = glmApiKey = glmVideoApiKey = deepseekApiKey = "";
        wanApiKey = klingApiKey = miniMaxApiKey = "";
        for (String provider : new String[] {"ark", "glm", "glm_video", "deepseek",
                "wan", "kling", "minimax"}) {
            String value = keys.optString(provider, "");
            if (value.isEmpty() || value.length() > 4096 ||
                value.indexOf('\r') >= 0 || value.indexOf('\n') >= 0) continue;
            switch (provider) {
            case "ark": arkApiKey = value; break;
            case "glm": glmApiKey = value; break;
            case "glm_video": glmVideoApiKey = value; break;
            case "deepseek": deepseekApiKey = value; break;
            case "wan": wanApiKey = value; break;
            case "kling": klingApiKey = value; break;
            case "minimax": miniMaxApiKey = value; break;
            default: break;
            }
        }
        apiKey = model.equals("deepseek-flash") ? deepseekApiKey : glmApiKey;
        String workspace = response.optString("wan_workspace_id", "");
        if (workspace.matches("(?:ws|llm)-[A-Za-z0-9_-]{1,120}")) wanWorkspaceId = workspace;
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
                emit();
            }
            ImportedFile result = imported;
            main.post(() -> completion.accept(result));
        });
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
        if (mime == null || !(mime.startsWith("image/") || mime.startsWith("video/")))
            throw new IllegalArgumentException("只能导入系统图库中的图片或视频");
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
                if (mime.startsWith("image/") && copied > 100L * 1024 * 1024)
                    throw new IllegalArgumentException("照片超过 100 MB");
                output.write(buffer, 0, count);
            }
            if (copied == 0) throw new IOException("媒体文件为空");
        } catch (Exception error) {
            target.delete();
            throw error;
        }
        return new ImportedFile(name, mime, true);
    }
    void importCameraFile(File source, Consumer<ImportedFile> completion) {
        worker.post(() -> {
            ImportedFile imported = null;
            try (InputStream input = new java.io.FileInputStream(source)) {
                imported = importStream(input, "image/jpeg", source.getName());
            } catch (Exception e) {
                error = safeMessage(e);
                emit();
            } finally {
                if (source != null) source.delete();
            }
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
                emit();
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
        Files.write(new File(new File(root, "Workspace"), name).toPath(), value);
        return new ImportedFile(name, mime, image);
    }
    private SecretKey encryptionKey() throws Exception {
        KeyStore store = KeyStore.getInstance("AndroidKeyStore");
        store.load(null);
        String alias = "MaiChat.AIAssistant.APIKey";
        if (store.containsAlias(alias))
            return ((KeyStore.SecretKeyEntry) store.getEntry(alias, null)).getSecretKey();
        KeyGenerator generator = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore");
        generator.init(new KeyGenParameterSpec
                .Builder(alias, KeyProperties.PURPOSE_ENCRYPT | KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .build());
        return generator.generateKey();
    }
    private void clearLegacyModelKeyFiles() {
        for (String name : new String[] {"api-key.enc", "ark-api-key.enc",
                "glm-main-api-key.enc", "glm-video-api-key.enc",
                "deepseek-main-api-key.enc", "wan-api-key.enc",
                "kling-api-key.enc", "minimax-api-key.enc"}) {
            new File(root, name).delete();
            new File(root, name + ".bak").delete();
            new File(root, name + ".new").delete();
        }
    }
    private void writeEncryptedKey(String name, String key) throws Exception {
        Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
        cipher.init(Cipher.ENCRYPT_MODE, encryptionKey());
        JSONObject value = new JSONObject()
                               .put("iv", Base64.encodeToString(cipher.getIV(), Base64.NO_WRAP))
                               .put("data",
                                   Base64.encodeToString(
                                       cipher.doFinal(key.getBytes(StandardCharsets.UTF_8)), Base64.NO_WRAP));
        android.util.AtomicFile file = new android.util.AtomicFile(new File(root, name));
        FileOutputStream out = file.startWrite();
        try {
            out.write(value.toString().getBytes(StandardCharsets.UTF_8));
            file.finishWrite(out);
        } catch (Exception e) {
            file.failWrite(out);
            throw e;
        }
    }
    private String readEncryptedKey(String name) throws Exception {
        File file = new File(root, name);
        if (!file.isFile())
            return "";
        JSONObject value =
            new JSONObject(new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8));
        Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
        cipher.init(Cipher.DECRYPT_MODE, encryptionKey(),
            new GCMParameterSpec(128, Base64.decode(value.getString("iv"), Base64.NO_WRAP)));
        return new String(
            cipher.doFinal(Base64.decode(value.getString("data"), Base64.NO_WRAP)), StandardCharsets.UTF_8);
    }
    private void exportSystemCertificates() throws Exception {
        TrustManagerFactory factory =
            TrustManagerFactory.getInstance(TrustManagerFactory.getDefaultAlgorithm());
        factory.init((KeyStore) null);
        StringBuilder pem = new StringBuilder();
        for (TrustManager manager : factory.getTrustManagers())
            if (manager instanceof X509TrustManager) {
                for (X509Certificate certificate : ((X509TrustManager) manager).getAcceptedIssuers()) {
                    pem.append("-----BEGIN CERTIFICATE-----\n")
                        .append(Base64.encodeToString(certificate.getEncoded(), Base64.NO_WRAP))
                        .append("\n-----END CERTIFICATE-----\n");
                }
            }
        if (pem.length() == 0)
            throw new IllegalStateException("无法读取系统信任证书");
        Files.write(
            new File(root, "trusted-roots.pem").toPath(), pem.toString().getBytes(StandardCharsets.US_ASCII));
    }
    void close() {
        listener = null;
        worker.post(() -> {
            closed = true;
            worker.removeCallbacks(poll);
            if (handle != 0)
                nativeDestroy(handle);
            handle = 0;
            thread.quitSafely();
        });
    }
}
