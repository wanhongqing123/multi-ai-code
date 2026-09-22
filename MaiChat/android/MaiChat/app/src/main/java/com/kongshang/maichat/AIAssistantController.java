package com.kongshang.maichat;

import android.content.Context;
import android.net.Uri;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;
import android.util.Base64;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.KeyStore;
import java.security.cert.X509Certificate;
import java.util.UUID;
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
final class AIAssistantController {
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
    private static AIAssistantController shared;
    static synchronized AIAssistantController shared(Context context) {
        if (shared == null)
            shared = new AIAssistantController(context, null);
        return shared;
    }
    private static native long nativeCreate();
    private static native void nativeDestroy(long handle);
    private static native byte[] nativeRequest(long handle, byte[] request);

    private final Context context;
    private final HandlerThread thread = new HandlerThread("MaiChat-Agent");
    private final Handler worker;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final String testEndpoint;
    private volatile Listener listener;
    volatile State state = new State(new JSONObject(), "", "https://open.bigmodel.cn/api/coding/paas/v4",
        "glm-5.3", "on-request", "", false);
    private long handle;
    private File root;
    private String selected = "", baseUrl = state.baseUrl, model = state.model, policy = state.policy,
                   error = "", apiKey = "";
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
    private void initialize() {
        try {
            System.loadLibrary("maichat_agent");
            handle = nativeCreate();
            root = new File(context.getNoBackupFilesDir(),
                testEndpoint == null ? "AIAssistant" : "AIAssistantTest-" + UUID.randomUUID());
            if (!root.mkdirs() && !root.isDirectory())
                throw new IllegalStateException("无法创建 AI 工作区");
            File settings = new File(root, "settings.json");
            if (settings.isFile()) {
                JSONObject saved =
                    new JSONObject(new String(Files.readAllBytes(settings.toPath()), StandardCharsets.UTF_8));
                baseUrl = saved.optString("baseUrl", baseUrl);
                model = saved.optString("model", model);
                policy = saved.optString("policy", policy);
            }
            try {
                apiKey = readKey();
            } catch (Exception e) {
                error = "API Key 无法读取，请重新配置模型";
            }
            if (testEndpoint != null) {
                baseUrl = testEndpoint;
                model = "test-model";
                apiKey = "test-key";
            }
            exportSystemCertificates();
            configure(baseUrl, model, policy, apiKey);
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
    private void configure(String url, String name, String approval, String key) throws Exception {
        File workspace = new File(root, "Workspace");
        if (!workspace.mkdirs() && !workspace.isDirectory())
            throw new IllegalStateException("无法创建工作区");
        call(op("configure")
                .put("baseUrl", url)
                .put("apiKey", key)
                .put("model", name)
                .put("policy", approval)
                .put("database", new File(root, "sessions.sqlite").getPath())
                .put("workspace", workspace.getPath())
                .put("caBundle", new File(root, "trusted-roots.pem").getPath()));
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
        String intendedSession = state.selected;
        worker.post(() -> {
            boolean success = false;
            try {
                String target = intendedSession;
                if (target.isEmpty()) {
                    target = call(op("create")).getString("id");
                    selected = target;
                }
                call(op("send").put("session", target).put("text", text));
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
    void save(String url, String name, String approval, String newKey, Consumer<Boolean> completion) {
        worker.post(() -> {
            boolean success = false;
            try {
                java.net.URI uri = new java.net.URI(url.trim());
                if (!"https".equals(uri.getScheme()) || uri.getHost() == null
                    || uri.getRawUserInfo() != null || uri.getRawQuery() != null
                    || uri.getRawFragment() != null || name.trim().isEmpty())
                    throw new IllegalArgumentException("请填写有效的 HTTPS API 地址和模型名称");
                if (newKey.trim().isEmpty()
                    && !java.util.Objects.equals(uri.getHost(), new java.net.URI(baseUrl).getHost()))
                    throw new IllegalArgumentException("更换模型服务商时，请重新填写 API Key");
                String key = newKey.trim().isEmpty() ? apiKey : newKey.trim();
                if (key.isEmpty())
                    throw new IllegalArgumentException("请填写 API Key");
                configure(url.trim(), name.trim(), approval, key);
                try {
                    writeKey(key);
                    byte[] config = new JSONObject()
                                        .put("baseUrl", url.trim())
                                        .put("model", name.trim())
                                        .put("policy", approval)
                                        .toString()
                                        .getBytes(StandardCharsets.UTF_8);
                    android.util.AtomicFile file =
                        new android.util.AtomicFile(new File(root, "settings.json"));
                    FileOutputStream out = file.startWrite();
                    try {
                        out.write(config);
                        file.finishWrite(out);
                    } catch (Exception e) {
                        file.failWrite(out);
                        throw e;
                    }
                } catch (Exception e) {
                    writeKey(apiKey);
                    configure(baseUrl, model, policy, apiKey);
                    throw e;
                }
                baseUrl = url.trim();
                model = name.trim();
                policy = approval;
                apiKey = key;
                error = "";
                success = true;
                refresh(true);
            } catch (Exception e) {
                error = safeMessage(e);
                emit();
            }
            boolean saved = success;
            main.post(() -> completion.accept(saved));
        });
    }
    void importFile(Uri uri, Consumer<String> completion) {
        worker.post(() -> {
            String name = null;
            try (InputStream input = context.getContentResolver().openInputStream(uri)) {
                if (input == null)
                    throw new IllegalArgumentException("无法读取文件");
                java.io.ByteArrayOutputStream bytes = new java.io.ByteArrayOutputStream();
                byte[] buffer = new byte[8192];
                int count;
                while ((count = input.read(buffer)) >= 0) {
                    if (bytes.size() + count > 5 * 1024 * 1024)
                        throw new IllegalArgumentException("请选择不超过 5 MB 的文本文件");
                    bytes.write(buffer, 0, count);
                }
                byte[] value = bytes.toByteArray();
                StandardCharsets.UTF_8.newDecoder().decode(java.nio.ByteBuffer.wrap(value));
                name = "import-" + UUID.randomUUID().toString().substring(0, 8) + ".txt";
                Files.write(new File(new File(root, "Workspace"), name).toPath(), value);
            } catch (Exception e) {
                error = safeMessage(e);
                emit();
            }
            String result = name;
            main.post(() -> completion.accept(result));
        });
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
    private void writeKey(String key) throws Exception {
        Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
        cipher.init(Cipher.ENCRYPT_MODE, encryptionKey());
        JSONObject value = new JSONObject()
                               .put("iv", Base64.encodeToString(cipher.getIV(), Base64.NO_WRAP))
                               .put("data",
                                   Base64.encodeToString(
                                       cipher.doFinal(key.getBytes(StandardCharsets.UTF_8)), Base64.NO_WRAP));
        android.util.AtomicFile file = new android.util.AtomicFile(new File(root, "api-key.enc"));
        FileOutputStream out = file.startWrite();
        try {
            out.write(value.toString().getBytes(StandardCharsets.UTF_8));
            file.finishWrite(out);
        } catch (Exception e) {
            file.failWrite(out);
            throw e;
        }
    }
    private String readKey() throws Exception {
        File file = new File(root, "api-key.enc");
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
