package com.kongshang.maichat;

import android.content.Context;
import android.content.SharedPreferences;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;
import android.util.Base64;

import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.security.KeyStore;

import javax.crypto.Cipher;
import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;
import javax.crypto.spec.GCMParameterSpec;

/** 手机号账号协议。刷新令牌只以 Android Keystore 加密形式落盘，UserSig 由后台签发。 */
public final class MaiChatAuthClient {
    public static final class Challenge {
        public final String id;
        public final int resendAfter;

        Challenge(String id, int resendAfter) {
            this.id = id;
            this.resendAfter = resendAfter;
        }
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

    private static final String BASE_URL = "https://ichat.life/api/v1/";
    private static final String KEY_ALIAS = "MaiChat.PhoneAuth.Refresh";
    private static final String STORED_REFRESH = "refresh";
    private final SharedPreferences preferences;
    private String accessToken = "";
    private long accessExpiresAt;

    public MaiChatAuthClient(Context context) {
        preferences = context.getApplicationContext()
            .getSharedPreferences("maichat-phone-auth", Context.MODE_PRIVATE);
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

    public synchronized String verifyCode(
        String phone, String challengeId, String code, boolean ageConfirmed)
        throws Exception {
        JSONObject body = new JSONObject().put("phone", phone)
            .put("challenge_id", challengeId).put("code", code)
            .put("age_confirmed_14_plus", ageConfirmed);
        return adopt(request("auth/sms/verify", "POST", body, ""));
    }

    public synchronized String loginWithPassword(
        String phone, String password, boolean ageConfirmed)
        throws Exception {
        JSONObject body = new JSONObject().put("phone", phone).put("password", password)
            .put("age_confirmed_14_plus", ageConfirmed);
        return adopt(request("auth/password/login", "POST", body, ""));
    }

    public synchronized Credential credential() throws Exception {
        JSONObject response;
        try {
            response = request("im/credential", "GET", null, currentAccessToken());
        } catch (IOException failure) {
            if (!String.valueOf(failure.getMessage()).startsWith("登录已过期")) throw failure;
            accessToken = "";
            response = request("im/credential", "GET", null, currentAccessToken());
        }
        Credential result = new Credential(
            response.getInt("sdk_app_id"), response.getString("user_id"),
            response.getString("user_sig"), response.getLong("expires_at"));
        if (result.sdkAppId <= 0 || result.userId.isEmpty() || result.userSig.isEmpty())
            throw new IOException("登录服务返回的 IM 凭证无效");
        return result;
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
        preferences.edit().remove(STORED_REFRESH).apply();
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
            if (String.valueOf(failure.getMessage()).startsWith("登录已过期")) clearLocalSession();
            throw failure;
        }
        adopt(response);
        return accessToken;
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

    private static JSONObject request(String path, String method, JSONObject body, String bearer)
        throws Exception {
        HttpURLConnection connection = (HttpURLConnection) new URL(BASE_URL + path).openConnection();
        try {
            connection.setRequestMethod(method);
            connection.setConnectTimeout(10_000);
            connection.setReadTimeout(20_000);
            connection.setRequestProperty("Accept", "application/json");
            connection.setRequestProperty("Cache-Control", "no-store");
            if (!bearer.isEmpty())
                connection.setRequestProperty("Authorization", "Bearer " + bearer);
            if (body != null) {
                byte[] bytes = body.toString().getBytes(StandardCharsets.UTF_8);
                connection.setDoOutput(true);
                connection.setRequestProperty("Content-Type", "application/json");
                try (OutputStream output = connection.getOutputStream()) { output.write(bytes); }
            }
            int status = connection.getResponseCode();
            InputStream input = status < 400
                ? connection.getInputStream() : connection.getErrorStream();
            ByteArrayOutputStream output = new ByteArrayOutputStream();
            if (input != null) {
                try (InputStream stream = input) {
                    byte[] buffer = new byte[4096];
                    int count;
                    while ((count = stream.read(buffer)) != -1) {
                        if (output.size() + count > 64 * 1024)
                            throw new IOException("登录服务响应过大");
                        output.write(buffer, 0, count);
                    }
                }
            }
            JSONObject result = new JSONObject(output.toString(StandardCharsets.UTF_8.name()));
            if (status < 200 || status >= 300)
                throw new IOException(errorText(result.optString("error"), status));
            return result;
        } finally {
            connection.disconnect();
        }
    }

    private static String errorText(String error, int status) {
        switch (error) {
            case "Invalid phone or purpose": return "手机号格式不正确";
            case "Please wait before requesting another code": return "请求太频繁，请稍后再试";
            case "Verification code could not be sent": return "验证码发送失败，请稍后再试";
            case "Invalid or expired verification code": return "验证码错误或已过期";
            case "Phone or password is incorrect": return "手机号或密码错误";
            case "Please wait before trying again": return "登录尝试过多，请稍后再试";
            case "Authentication required": return "登录已过期，请重新用手机号登录";
            case "Invalid session": return "登录已过期，请重新用手机号登录";
            case "Minimum age confirmation required": return "请先确认已年满 14 周岁";
            default: return "登录服务暂时不可用（" + status + "）";
        }
    }
}
