// 文件作用：Android IM 身份、消息同步与本地历史。
// MaiChatAuthClient 负责外部接口请求与结果转换，本文件给出其实现。
// 主要入口：shared、hasSession、requestCode、refresh、credential。
package com.kongshang.maichat;

import android.content.Context;
import android.content.SharedPreferences;
import android.os.Build;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;
import android.util.Base64;

import org.json.JSONObject;

import java.io.IOException;
import java.net.URI;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.security.KeyStore;
import java.util.UUID;

import javax.crypto.Cipher;
import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;
import javax.crypto.spec.GCMParameterSpec;

/** 手机号账号协议。刷新令牌只以 Android Keystore 加密形式落盘，UserSig 由后台签发。 */
public final class MaiChatAuthClient {
    private static volatile MaiChatAuthClient shared;

    public static MaiChatAuthClient shared(Context context) {
        MaiChatAuthClient current = shared;
        if (current != null) return current;
        synchronized (MaiChatAuthClient.class) {
            if (shared == null) shared = new MaiChatAuthClient(context);
            return shared;
        }
    }

    public static final class Challenge {
        public final String id;
        public final int resendAfter;

        Challenge(String id, int resendAfter) {
            this.id = id;
            this.resendAfter = resendAfter;
        }
    }

    public static final class SocialLoginResult {
        public final String userId;
        public final String bindingToken;
        public final int expiresIn;

        private SocialLoginResult(String userId, String bindingToken, int expiresIn) {
            this.userId = userId;
            this.bindingToken = bindingToken;
            this.expiresIn = expiresIn;
        }

        public boolean isSignedIn() { return !userId.isEmpty(); }
        public boolean needsPhoneBinding() { return !bindingToken.isEmpty(); }
    }

    public static final class Credential {
        public final int sdkAppId;
        public final String userId;
        public final String userSig;
        public final long expiresAt;

        Credential(int sdkAppId, String userId, String userSig, long expiresAt) {
            this.sdkAppId = sdkAppId;
            this.userId = userId;
            this.userSig = userSig;
            this.expiresAt = expiresAt;
        }
    }

    public static final class ModelCredential {
        public final String apiKey;
        public final long expiresAt;

        ModelCredential(String apiKey, long expiresAt) {
            this.apiKey = apiKey;
            this.expiresAt = expiresAt;
        }
    }

    public static final class BailianCredential {
        public final String apiKey;
        public final long expiresAt;
        public final String workspaceId;

        BailianCredential(String apiKey, long expiresAt, String workspaceId) {
            this.apiKey = apiKey;
            this.expiresAt = expiresAt;
            this.workspaceId = workspaceId;
        }
    }

    public static final class AccountModelKeys {
        public final String glm;
        public final String deepseek;
        public final String minimax;

        AccountModelKeys(String glm, String deepseek, String minimax) {
            this.glm = glm;
            this.deepseek = deepseek;
            this.minimax = minimax;
        }
    }

    public static final class RealtimeAsrTicket {
        public final String voiceId;
        public final String url;
        public final long expiresAt;

        private RealtimeAsrTicket(String voiceId, String url, long expiresAt) {
            this.voiceId = voiceId;
            this.url = url;
            this.expiresAt = expiresAt;
        }
    }

    public static final class MediaServiceAccess {
        public final String baseUrl;
        public final String accountToken;

        MediaServiceAccess(String baseUrl, String accountToken) {
            this.baseUrl = baseUrl;
            this.accountToken = accountToken;
        }
    }

    public static final class DesktopRequestInfo {
        public final String platform;
        public final String displayCode;
        public final String status;
        public final int expiresIn;

        DesktopRequestInfo(String platform, String displayCode, String status, int expiresIn) {
            this.platform = platform;
            this.displayCode = displayCode;
            this.status = status;
            this.expiresIn = expiresIn;
        }

        public boolean isAwaitingDecision() {
            return ("pending".equals(status) || "scanned".equals(status)) && expiresIn > 0;
        }
    }

    public static class SessionTerminatedException extends IOException {
        SessionTerminatedException(String message) { super(message); }
    }

    public static final class SessionReplacedException extends SessionTerminatedException {
        SessionReplacedException() { super("此设备的登录已被另一台同类设备替换，请重新登录"); }
    }

    public static final class DeviceSlotRequiredException extends SessionTerminatedException {
        DeviceSlotRequiredException() { super("请更新 App 后重新登录"); }
    }

    public interface SessionTerminationListener {
        void onSessionTerminated(String message);
    }

    // 只有显式开启的 Debug 模拟器连接本机假账号服务；普通调试和发布包使用正式地址。
    private static final String BASE_URL = BuildConfig.DEBUG && BuildConfig.USE_LOCAL_UI_AUTH &&
        (Build.FINGERPRINT.contains("sdk_gphone") || Build.FINGERPRINT.contains("generic"))
        ? "http://10.0.2.2:18190/v1/" : "https://ichat.life/api/v1/";
    private static final String KEY_ALIAS = "MaiChat.PhoneAuth.Refresh";
    private static final String STORED_REFRESH = "refresh";
    private final SharedPreferences preferences;
    private String accessToken = "";
    private long accessExpiresAt;
    private volatile SessionTerminationListener terminationListener;
    private final boolean tablet;

    public MaiChatAuthClient(Context context) {
        preferences = context.getApplicationContext()
            .getSharedPreferences("maichat-phone-auth", Context.MODE_PRIVATE);
        tablet = context.getResources().getConfiguration().smallestScreenWidthDp >= 600;
    }

    public void setSessionTerminationListener(SessionTerminationListener listener) {
        terminationListener = listener;
    }

    private JSONObject addDevice(JSONObject body) throws Exception {
        body.put("device_slot", tablet ? "tablet" : "android_phone");
        body.put("device_platform", "android");
        return body;
    }

    public synchronized boolean hasSession() {
        try { return !readRefreshToken().isEmpty(); }
        catch (Exception error) { return false; }
    }

    public Challenge requestCode(String phone, boolean ageConfirmed) throws Exception {
        JSONObject body = new JSONObject().put("phone", phone).put("purpose", "login")
            .put("age_confirmed_14_plus", ageConfirmed);
        JSONObject response = request("auth/sms/challenge", "POST", body, "");
        return new Challenge(response.getString("challenge_id"), response.getInt("resend_after"));
    }

    public Challenge requestSocialBindCode(String phone, boolean ageConfirmed) throws Exception {
        JSONObject body = new JSONObject().put("phone", phone).put("purpose", "social_bind")
            .put("age_confirmed_14_plus", ageConfirmed);
        JSONObject response = request("auth/sms/challenge", "POST", body, "");
        return new Challenge(response.getString("challenge_id"), response.getInt("resend_after"));
    }

    public synchronized String verifyCode(
        String phone, String challengeId, String code, boolean ageConfirmed)
        throws Exception {
        JSONObject body = addDevice(new JSONObject().put("phone", phone)
            .put("challenge_id", challengeId).put("code", code)
            .put("age_confirmed_14_plus", ageConfirmed));
        return adopt(request("auth/sms/verify", "POST", body, ""));
    }

    public synchronized String loginWithPassword(
        String phone, String password, boolean ageConfirmed)
        throws Exception {
        JSONObject body = addDevice(new JSONObject().put("phone", phone).put("password", password)
            .put("age_confirmed_14_plus", ageConfirmed));
        return adopt(request("auth/password/login", "POST", body, ""));
    }

    public synchronized SocialLoginResult loginWithSocialCredential(
        String provider, String credential, boolean ageConfirmed) throws Exception {
        String path;
        String credentialKey;
        if ("wechat".equals(provider)) {
            path = "auth/social/wechat/login";
            credentialKey = "code";
        } else if ("qq".equals(provider)) {
            path = "auth/social/qq/login";
            credentialKey = "access_token";
        } else {
            throw new IllegalArgumentException("不支持的登录方式");
        }
        JSONObject body = addDevice(new JSONObject().put(credentialKey, credential)
            .put("age_confirmed_14_plus", ageConfirmed));
        HttpResponse response = requestWithStatus(path, "POST", body, "");
        if (response.status == 200)
            return new SocialLoginResult(adopt(response.body), "", 0);
        if (response.status == 202) {
            String token = response.body.optString("binding_token");
            int expiresIn = response.body.optInt("expires_in");
            if (!response.body.optBoolean("phone_binding_required") || token.isEmpty()
                || expiresIn <= 0)
                throw new IOException("登录服务返回的手机绑定数据无效");
            return new SocialLoginResult("", token, expiresIn);
        }
        throw new IOException("登录服务返回的社交登录状态无效");
    }

    public synchronized String bindSocialPhone(
        String bindingToken, String phone, String challengeId, String code,
        boolean ageConfirmed) throws Exception {
        JSONObject body = new JSONObject().put("binding_token", bindingToken)
            .put("phone", phone).put("challenge_id", challengeId).put("code", code)
            .put("age_confirmed_14_plus", ageConfirmed);
        return adopt(request("auth/social/bind", "POST", body, ""));
    }

    public synchronized String resetPassword(
        String phone, String challengeId, String code, String newPassword,
        boolean ageConfirmed) throws Exception {
        JSONObject body = addDevice(new JSONObject().put("phone", phone)
            .put("challenge_id", challengeId).put("code", code)
            .put("new_password", newPassword)
            .put("age_confirmed_14_plus", ageConfirmed));
        return adopt(request("auth/password/reset", "POST", body, ""));
    }

    public synchronized DesktopRequestInfo desktopRequestInfo(String requestId) throws Exception {
        String id = canonicalRequestId(requestId);
        JSONObject response = authenticatedRequest("auth/desktop/request/" + id, "GET", null);
        DesktopRequestInfo info = new DesktopRequestInfo(response.getString("platform"),
            response.getString("display_code"), response.getString("status"),
            response.getInt("expires_in"));
        if (!(info.platform.equals("mac") || info.platform.equals("windows"))
            || !info.displayCode.matches("[0-9]{6}") || info.expiresIn < 0)
            throw new IOException("桌面登录请求数据无效");
        return info;
    }

    public synchronized void decideDesktopRequest(
        String requestId, String expectedCode, boolean approve) throws Exception {
        String id = canonicalRequestId(requestId);
        DesktopRequestInfo info = desktopRequestInfo(id);
        if (!info.displayCode.equals(expectedCode))
            throw new IOException("电脑上的核对码已变化，请重新扫码");
        if (!info.isAwaitingDecision())
            throw new IOException("该二维码已过期或已处理，请在电脑上刷新");
        JSONObject result = authenticatedRequest(approve ? "auth/desktop/approve" :
            "auth/desktop/deny", "POST", new JSONObject().put("request_id", id));
        if (!result.optString("status").equals(approve ? "approved" : "denied"))
            throw new IOException("桌面登录结果无效，请重试");
    }

    private static String canonicalRequestId(String requestId) throws IOException {
        if (requestId == null || !requestId.matches(
            "[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}"))
            throw new IOException("不是有效的桌面登录二维码");
        return UUID.fromString(requestId).toString();
    }

    public synchronized Credential credential() throws Exception {
        JSONObject response;
        try {
            response = authenticatedRequest("im/credential", "GET", null);
        } catch (IOException failure) {
            if (!String.valueOf(failure.getMessage()).startsWith("登录已过期")) throw failure;
            accessToken = "";
            response = authenticatedRequest("im/credential", "GET", null);
        }
        Credential result = new Credential(
            response.getInt("sdk_app_id"), response.getString("user_id"),
            response.getString("user_sig"), response.getLong("expires_at"));
        if (result.sdkAppId <= 0 || result.userId.isEmpty() || result.userSig.isEmpty())
            throw new IOException("登录服务返回的 IM 凭证无效");
        return result;
    }

    public synchronized ModelCredential seedanceCredential() throws Exception {
        if (!hasSession()) return null;
        JSONObject response;
        try {
            response = authenticatedRequest("model/seedance-credential", "GET", null);
        } catch (IOException failure) {
            if (!String.valueOf(failure.getMessage()).startsWith("登录已过期")) throw failure;
            accessToken = "";
            response = authenticatedRequest("model/seedance-credential", "GET", null);
        }
        ModelCredential result = new ModelCredential(
            response.getString("api_key"), response.getLong("expires_at"));
        long now = System.currentTimeMillis() / 1000L;
        if (result.apiKey.length() < 16 || result.expiresAt <= now + 30 ||
            result.expiresAt > now + 360)
            throw new IOException("Seedance 临时凭据无效");
        return result;
    }

    public synchronized BailianCredential bailianCredential() throws Exception {
        if (!hasSession()) return null;
        JSONObject response;
        try {
            response = authenticatedRequest("model/bailian-credential", "GET", null);
        } catch (IOException failure) {
            if (!String.valueOf(failure.getMessage()).startsWith("登录已过期")) throw failure;
            accessToken = "";
            response = authenticatedRequest("model/bailian-credential", "GET", null);
        }
        BailianCredential result = new BailianCredential(
            response.getString("api_key"), response.getLong("expires_at"),
            response.getString("wan_workspace_id"));
        long now = System.currentTimeMillis() / 1000L;
        if (!result.apiKey.matches("st-[A-Za-z0-9._~+/-]{8,4093}={0,2}") ||
            result.expiresAt <= now + 30 || result.expiresAt > now + 360 ||
            !result.workspaceId.matches("(?:ws|llm)-[A-Za-z0-9_-]{1,120}"))
            throw new IOException("百炼临时凭据无效");
        return result;
    }

    public synchronized AccountModelKeys accountModelKeys() throws Exception {
        if (!hasSession()) return null;
        JSONObject keys = authenticatedRequest("model/api-keys", "GET", null)
            .getJSONObject("api_keys");
        String glm = keys.getString("glm");
        String deepseek = keys.getString("deepseek");
        String minimax = keys.optString("minimax", "");
        if ((glm.isEmpty() && deepseek.isEmpty() && minimax.isEmpty()) ||
            !validModelKey(glm) || !validModelKey(deepseek) || !validModelKey(minimax))
            throw new IOException("主聊天模型凭据无效");
        return new AccountModelKeys(glm, deepseek, minimax);
    }

    public synchronized RealtimeAsrTicket realtimeAsrTicket() throws Exception {
        if (!hasSession()) throw new IOException("请先登录后使用语音转文字");
        JSONObject issued = authenticatedRequest("asr/realtime-url", "POST", new JSONObject());
        return parseRealtimeAsrTicket(issued, System.currentTimeMillis() / 1000L);
    }

    static RealtimeAsrTicket parseRealtimeAsrTicket(JSONObject issued, long now) throws IOException {
        String voiceId = issued.optString("voice_id", "");
        String url = issued.optString("url", "");
        long expiresAt = issued.optLong("expires_at", 0);
        if (!voiceId.matches("[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}") ||
            url.length() > 4096 || expiresAt <= now + 10 || expiresAt > now + 360)
            throw new IOException("语音识别签名无效");
        try {
            URI endpoint = new URI(url);
            String query = endpoint.getRawQuery();
            if (!"wss".equals(endpoint.getScheme()) ||
                !"asr.cloud.tencent.com".equals(endpoint.getHost()) ||
                endpoint.getPort() != -1 || endpoint.getUserInfo() != null ||
                endpoint.getFragment() != null ||
                !endpoint.getPath().matches("/asr/v2/[0-9]{5,20}") ||
                query == null || !voiceId.equals(queryValue(query, "voice_id")) ||
                !"16k_zh".equals(queryValue(query, "engine_model_type")) ||
                !"1".equals(queryValue(query, "needvad")) ||
                !"16".equals(queryValue(query, "voice_format")) ||
                !Long.toString(expiresAt).equals(queryValue(query, "expired")) ||
                queryValue(query, "signature").isEmpty())
                throw new IOException("语音识别签名无效");
            return new RealtimeAsrTicket(voiceId, url, expiresAt);
        } catch (java.net.URISyntaxException error) {
            throw new IOException("语音识别签名无效", error);
        }
    }

    private static String queryValue(String query, String key) {
        for (String item : query.split("&")) {
            int separator = item.indexOf('=');
            if (separator > 0 && item.substring(0, separator).equals(key))
                return item.substring(separator + 1);
        }
        return "";
    }

    public synchronized MediaServiceAccess mediaServiceAccess() throws Exception {
        if (!hasSession()) return null;
        String token = currentAccessToken();
        String baseUrl = new URL(new URL(BASE_URL), "/maichat").toString();
        return new MediaServiceAccess(baseUrl, token);
    }

    private static boolean validModelKey(String key) {
        if (key.length() > 4096) return false;
        for (int index = 0; index < key.length(); index++) {
            char character = key.charAt(index);
            if (character < 32 || character == 127) return false;
        }
        return true;
    }

    /** 先同步撤销本机登录状态，避免用户点退出后进程立即结束又自动登录。 */
    public synchronized String discardLocalSession() {
        String token = accessToken;
        clearLocalSession();
        return token;
    }

    public static void revokeServerSession(String accessToken) {
        if (accessToken == null || accessToken.isEmpty()) return;
        try {
            request("auth/logout", "POST", new JSONObject(), accessToken);
        } catch (Exception ignored) {
            // 断网时本机已退出，服务端会话按自身过期时间回收。
        }
    }

    private void clearLocalSession() {
        accessToken = "";
        accessExpiresAt = 0;
        preferences.edit().remove(STORED_REFRESH).commit();
    }

    private String adopt(JSONObject session) throws Exception {
        String refresh = session.getString("refresh_token");
        String access = session.getString("access_token");
        String userId = session.getJSONObject("user").getString("im_user_id");
        if (refresh.isEmpty() || access.isEmpty() || userId.isEmpty())
            throw new IOException("登录服务返回的账号数据无效");
        writeRefreshToken(refresh);
        accessToken = access;
        accessExpiresAt = System.currentTimeMillis()
            + Math.max(0, session.getLong("expires_in") - 30) * 1000;
        return userId;
    }

    private String currentAccessToken() throws Exception {
        if (!accessToken.isEmpty() && System.currentTimeMillis() < accessExpiresAt)
            return accessToken;
        String refresh = readRefreshToken();
        if (refresh.isEmpty()) throw new IOException("登录已过期，请重新用手机号登录");
        JSONObject response;
        try {
            response = request("auth/refresh", "POST",
                new JSONObject().put("refresh_token", refresh), "");
        } catch (IOException failure) {
            if (failure instanceof SessionTerminatedException)
                handleSessionTerminated((SessionTerminatedException) failure);
            else if (String.valueOf(failure.getMessage()).startsWith("登录已过期")) clearLocalSession();
            throw failure;
        }
        adopt(response);
        return accessToken;
    }

    private JSONObject authenticatedRequest(String path, String method, JSONObject body)
        throws Exception {
        String bearer = currentAccessToken();
        try { return request(path, method, body, bearer); }
        catch (SessionTerminatedException failure) {
            handleSessionTerminated(failure);
            throw failure;
        } catch (IOException failure) {
            if (!String.valueOf(failure.getMessage()).startsWith("登录已过期")) throw failure;
            accessToken = "";
            bearer = currentAccessToken();
            try { return request(path, method, body, bearer); }
            catch (SessionTerminatedException terminated) {
                handleSessionTerminated(terminated);
                throw terminated;
            }
        }
    }

    public synchronized void validateSession() throws Exception {
        authenticatedRequest("auth/me", "GET", null);
    }

    private void handleSessionTerminated(SessionTerminatedException failure) {
        clearLocalSession();
        SessionTerminationListener listener = terminationListener;
        if (listener != null) listener.onSessionTerminated(failure.getMessage());
    }

    private SecretKey encryptionKey() throws Exception {
        KeyStore store = KeyStore.getInstance("AndroidKeyStore");
        store.load(null);
        if (store.containsAlias(KEY_ALIAS))
            return ((KeyStore.SecretKeyEntry) store.getEntry(KEY_ALIAS, null)).getSecretKey();
        KeyGenerator generator = KeyGenerator.getInstance(
            KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore");
        generator.init(new KeyGenParameterSpec.Builder(
            KEY_ALIAS, KeyProperties.PURPOSE_ENCRYPT | KeyProperties.PURPOSE_DECRYPT)
            .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
            .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
            .build());
        return generator.generateKey();
    }

    private void writeRefreshToken(String value) throws Exception {
        Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
        cipher.init(Cipher.ENCRYPT_MODE, encryptionKey());
        byte[] iv = cipher.getIV();
        byte[] encoded = cipher.doFinal(value.getBytes(StandardCharsets.UTF_8));
        byte[] packed = new byte[iv.length + encoded.length];
        System.arraycopy(iv, 0, packed, 0, iv.length);
        System.arraycopy(encoded, 0, packed, iv.length, encoded.length);
        if (!preferences.edit().putString(STORED_REFRESH,
                Base64.encodeToString(packed, Base64.NO_WRAP)).commit())
            throw new IOException("登录状态保存失败");
    }

    private String readRefreshToken() throws Exception {
        String raw = preferences.getString(STORED_REFRESH, "");
        if (raw == null || raw.isEmpty()) return "";
        byte[] packed = Base64.decode(raw, Base64.NO_WRAP);
        if (packed.length <= 12) throw new IOException("登录状态无效");
        Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
        cipher.init(Cipher.DECRYPT_MODE, encryptionKey(),
            new GCMParameterSpec(128, packed, 0, 12));
        return new String(cipher.doFinal(packed, 12, packed.length - 12), StandardCharsets.UTF_8);
    }

    private static final class HttpResponse {
        final int status;
        final JSONObject body;

        HttpResponse(int status, JSONObject body) {
            this.status = status;
            this.body = body;
        }
    }

    private static JSONObject request(String path, String method, JSONObject body, String bearer)
        throws Exception {
        return requestWithStatus(path, method, body, bearer).body;
    }

    private static HttpResponse requestWithStatus(
        String path, String method, JSONObject body, String bearer)
        throws Exception {
        URL requestUrl = new URL(BASE_URL + path);
        byte[] requestBody = body == null ? null : body.toString().getBytes(StandardCharsets.UTF_8);
        return executeRequest(requestUrl, method, requestBody, bearer);
    }

    private static HttpResponse executeRequest(
        URL requestUrl, String method, byte[] requestBody, String bearer)
        throws Exception {
        MaiChatAuthTransport.Result response =
            MaiChatAuthTransport.perform(requestUrl, method, requestBody, bearer);
        JSONObject result = new JSONObject(new String(response.body, StandardCharsets.UTF_8));
        SessionTerminatedException terminated = terminalSessionError(
            response.status, result.optString("code"));
        if (terminated != null) throw terminated;
        if (response.status < 200 || response.status >= 300)
            throw new IOException(errorText(result.optString("error"),
                result.optString("code"), response.status));
        return new HttpResponse(response.status, result);
    }

    static SessionTerminatedException terminalSessionError(int status, String code) {
        if (status != 401) return null;
        if ("SESSION_REPLACED".equals(code)) return new SessionReplacedException();
        if ("DEVICE_SLOT_REQUIRED".equals(code)) return new DeviceSlotRequiredException();
        return null;
    }

    static String errorText(String error, String code, int status) {
        if ("MOBILE_SESSION_REQUIRED".equals(code)) return "请重新用手机号登录后扫码";
        if ("SOCIAL_PROVIDER_UNAVAILABLE".equals(code)) return "社交登录暂时不可用，请稍后再试";
        if ("SOCIAL_CREDENTIAL_INVALID".equals(code)) return "授权已失效，请重新授权";
        if ("SOCIAL_BINDING_EXPIRED".equals(code)) return "绑定已过期，请重新授权";
        if ("SOCIAL_IDENTITY_ALREADY_BOUND".equals(code)) return "该社交账号已绑定其他手机号";
        if ("SOCIAL_PROVIDER_ALREADY_BOUND".equals(code)) return "该手机号已绑定其他同类社交账号";
        switch (error) {
            case "Invalid phone or purpose": return "手机号格式不正确";
            case "Please wait before requesting another code": return "请求太频繁，请稍后再试";
            case "Verification code could not be sent": return "验证码发送失败，请稍后再试";
            case "Invalid or expired verification code": return "验证码错误或已过期";
            case "Phone or password is incorrect": return "手机号或密码错误";
            case "Please wait before trying again": return "登录尝试过多，请稍后再试";
            case "Authentication required": return "登录已过期，请重新用手机号登录";
            case "Invalid session": return "登录已过期，请重新用手机号登录";
            case "Model access is not enabled for this account":
                return "当前账号未开通模型使用权限";
            case "Desktop login request not found":
                return "未找到桌面登录请求，请在电脑上刷新二维码";
            case "Desktop login request expired": return "二维码已过期，请在电脑上刷新";
            case "Desktop login request already decided": return "该桌面登录请求已处理";
            case "Invalid social login request": return "授权数据无效，请重新授权";
            case "Invalid social binding request": return "绑定数据无效，请重新授权";
            case "Account unavailable": return "该账号暂时无法登录";
            case "Minimum age confirmation required": return "请先确认已年满 14 周岁";
            default: return "登录服务暂时不可用（" + status + "）";
        }
    }
}
