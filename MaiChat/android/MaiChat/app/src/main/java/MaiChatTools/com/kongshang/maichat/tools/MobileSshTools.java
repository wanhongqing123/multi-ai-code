package com.kongshang.maichat.tools;

import android.app.AlertDialog;
import android.content.Context;
import android.text.InputType;
import android.widget.EditText;
import com.kongshang.maichat.MainActivity;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import org.json.JSONObject;

/** Native credential and host-key dialogs for the shared MaiAgent SSH transport. */
final class MobileSshTools {
    private final MainActivity activity;

    MobileSshTools(MainActivity activity) {
        this.activity = activity;
    }

    JSONObject execute(String tool, JSONObject arguments) throws Exception {
        String host = arguments.optString("host", "").trim();
        int port = arguments.optInt("port", 22);
        if (host.isEmpty() || port < 1 || port > 65535)
            throw new IllegalArgumentException("SSH host or port is invalid");
        if (!activity.isHostToolForeground())
            throw new IllegalStateException("Open MaiChat before using SSH");
        if (tool.equals("mobile_ssh_password")) {
            String username = arguments.optString("username", "").trim();
            if (username.isEmpty())
                throw new IllegalArgumentException("SSH username is required");
            String password = requestPassword(host, username);
            if (password == null)
                throw new IllegalStateException("SSH password entry was canceled");
            return new JSONObject().put("password", password);
        }
        if (tool.equals("mobile_ssh_trust_host")) {
            String fingerprint = arguments.optString("fingerprint", "");
            if (!fingerprint.startsWith("SHA256:"))
                throw new IllegalArgumentException("SSH host fingerprint is invalid");
            String trustKey = "ssh.host.sha256." + host + ":" + port;
            String trusted = activity.getPreferences(Context.MODE_PRIVATE)
                .getString(trustKey, null);
            if (trusted != null)
                return new JSONObject().put("trusted", trusted.equals(fingerprint));
            boolean accepted = requestHostTrust(host, port, fingerprint);
            if (accepted) activity.getPreferences(Context.MODE_PRIVATE).edit()
                .putString(trustKey, fingerprint).apply();
            return new JSONObject().put("trusted", accepted);
        }
        throw new IllegalArgumentException("Unknown SSH host callback");
    }

    private String requestPassword(String host, String username) throws InterruptedException {
        final String[] value = new String[1];
        CountDownLatch done = new CountDownLatch(1);
        activity.runOnUiThread(() -> {
            if (!activity.isHostToolForeground()) { done.countDown(); return; }
            EditText field = new EditText(activity);
            field.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD);
            field.setSingleLine(true);
            new AlertDialog.Builder(activity)
                .setTitle("SSH 登录：" + username + "@" + host)
                .setMessage("密码只用于本次连接，不会发送给 AI 或保存在聊天记录中。")
                .setView(field)
                .setPositiveButton("连接", (dialog, which) -> {
                    value[0] = field.getText().toString();
                    done.countDown();
                })
                .setNegativeButton("取消", (dialog, which) -> done.countDown())
                .setOnCancelListener(dialog -> done.countDown())
                .show();
        });
        if (!done.await(120, TimeUnit.SECONDS)) return null;
        return value[0];
    }

    private boolean requestHostTrust(String host, int port, String fingerprint)
            throws InterruptedException {
        final boolean[] accepted = new boolean[1];
        CountDownLatch done = new CountDownLatch(1);
        activity.runOnUiThread(() -> {
            if (!activity.isHostToolForeground()) { done.countDown(); return; }
            new AlertDialog.Builder(activity)
                .setTitle("确认 SSH 服务器身份")
                .setMessage(host + ":" + port + "\n" + fingerprint
                    + "\n请与云主机控制台中的指纹核对，确认一致后再连接。")
                .setPositiveButton("指纹一致", (dialog, which) -> {
                    accepted[0] = true;
                    done.countDown();
                })
                .setNegativeButton("取消", (dialog, which) -> done.countDown())
                .setOnCancelListener(dialog -> done.countDown())
                .show();
        });
        return done.await(120, TimeUnit.SECONDS) && accepted[0];
    }
}
