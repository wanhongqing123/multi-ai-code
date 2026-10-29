// 文件作用：原生 PDF 密码双输入页及私有一次性回调，不注册成模型可调用的工具。
package com.kongshang.maichat.tools;

import android.app.AlertDialog;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.text.Editable;
import android.text.InputType;
import android.text.TextWatcher;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;
import com.kongshang.maichat.MainActivity;
import com.kongshang.maichat.RemoteIMSessionController;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.UUID;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import java.util.concurrent.atomic.AtomicBoolean;
import org.json.JSONObject;

/** 密码仅保留到第一次 poll 取走；关闭、账号变化、超时和原生取消均清除本次持有的引用。 */
public final class MobilePdfPasswordRequests {
    private static final class Request {
        final String id;
        final String identity;
        String state = "pending";
        String password;
        AlertDialog dialog;
        Request(String id, String identity) { this.id = id; this.identity = identity; }
    }
    private final MainActivity activity;
    private final RemoteIMSessionController session;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final LinkedHashMap<String, Request> requests = new LinkedHashMap<>();
    private String activeId;

    public MobilePdfPasswordRequests(MainActivity activity, RemoteIMSessionController session) {
        this.activity = activity;
        this.session = session;
    }

    /** 只供核心私有密码 provider；公开 Agent schema 没有此 action，也没有任何密码字段。 */
    public JSONObject execute(JSONObject arguments) throws Exception {
        Iterator<String> keys = arguments.keys();
        while (keys.hasNext()) {
            String key = keys.next();
            if (!key.equals("action") && !key.equals("request_id"))
                throw new IllegalArgumentException("Invalid private PDF password input");
        }
        Object actionValue = arguments.opt("action"), idValue = arguments.opt("request_id");
        if (!(actionValue instanceof String) || !(idValue instanceof String))
            throw new IllegalArgumentException("Invalid private PDF password input");
        String action = (String) actionValue;
        String rawId = (String) idValue;
        String id = UUID.fromString(rawId).toString();
        if (!id.equalsIgnoreCase(rawId) || !(action.equals("begin") || action.equals("poll") || action.equals("cancel")))
            throw new IllegalArgumentException("Invalid private PDF password action");
        if (Looper.myLooper() == Looper.getMainLooper()) return perform(action, id);
        AtomicReference<JSONObject> result = new AtomicReference<>();
        AtomicReference<Exception> failure = new AtomicReference<>();
        AtomicBoolean abandoned = new AtomicBoolean();
        CountDownLatch ready = new CountDownLatch(1);
        main.post(() -> {
            try {
                if (!abandoned.get()) result.set(perform(action, id));
            }
            catch (Exception error) { failure.set(error); }
            finally { ready.countDown(); }
        });
        try {
            if (!ready.await(5, TimeUnit.SECONDS)) {
                abandoned.set(true);
                // begin 可能恰好已进入 UI 线程；超时仍精确取消本 token，不能留残余表单。
                main.post(() -> cancel(id));
                throw new IllegalStateException("PDF password form unavailable");
            }
        } catch (InterruptedException interrupted) {
            abandoned.set(true);
            main.post(() -> cancel(id));
            throw interrupted;
        }
        if (failure.get() != null) throw failure.get();
        return result.get();
    }

    private JSONObject perform(String action, String id) throws Exception {
        if (action.equals("cancel")) { cancel(id); return state("canceled"); }
        Request request = requests.get(id);
        if (action.equals("poll")) {
            if (request == null || !request.identity.equals(session.passwordChangeIdentity())) {
                cancel(id);
                return state("canceled");
            }
            if (request.state.equals("ready")) {
                String password = request.password;
                request.password = null;
                request.state = "canceled";
                // 只有私有 provider 消费此响应；永不转成公开工具结果或诊断文本。
                return state("ready").put("password", password);
            }
            return state(request.state);
        }
        if (!activity.isHostToolForeground() || session.requiresLogin())
            throw new IllegalStateException("Open MaiChat before protecting a PDF");
        if (request != null) {
            if (!request.identity.equals(session.passwordChangeIdentity())) { cancel(id); return state("canceled"); }
            return state(request.state.equals("canceled") ? "canceled" : "pending");
        }
        if (activeId != null) throw new IllegalStateException("Another PDF password form is open");
        request = new Request(id, session.passwordChangeIdentity());
        requests.put(id, request);
        while (requests.size() > 20) requests.remove(requests.keySet().iterator().next());
        activeId = id;
        try {
            show(request);
        } catch (RuntimeException unavailable) {
            // Activity 退出时 show 可能失败；清掉刚登记的 activeId，避免以后永远显示 busy。
            cancel(id);
            throw new IllegalStateException("PDF password form unavailable");
        }
        main.postDelayed(() -> cancel(id), 120_000);
        return state("pending");
    }

    private void show(Request request) {
        LinearLayout form = new LinearLayout(activity);
        form.setOrientation(LinearLayout.VERTICAL);
        int padding = Math.round(20 * activity.getResources().getDisplayMetrics().density);
        form.setPadding(padding, padding / 2, padding, padding / 2);
        EditText password = new EditText(activity), confirmation = new EditText(activity);
        EditText[] fields = {password, confirmation};
        for (EditText field : fields) {
            field.setSingleLine(true);
            field.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD);
            field.setSaveEnabled(false);
            if (Build.VERSION.SDK_INT >= 26) field.setAutofillHints("newPassword");
            form.addView(field, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));
        }
        password.setHint("PDF 打开密码");
        confirmation.setHint("再次输入 PDF 密码");
        TextView help = new TextView(activity);
        help.setText("仅设置本次 PDF 的打开密码，不修改 MaiChat 登录密码。最多 127 个 UTF-8 字节，不会发给 AI 或保存在聊天记录中。");
        form.addView(help);
        AlertDialog dialog = new AlertDialog.Builder(activity).setTitle("PDF 密码保护")
            .setView(form).setNegativeButton("取消", (ignored, which) -> cancel(request.id))
            .setPositiveButton("保护 PDF", null).create();
        request.dialog = dialog;
        dialog.setOnShowListener(ignored -> {
            Button accept = dialog.getButton(AlertDialog.BUTTON_POSITIVE);
            accept.setEnabled(false);
            TextWatcher changed = new TextWatcher() {
                @Override public void beforeTextChanged(CharSequence s, int start, int count, int after) {}
                @Override public void onTextChanged(CharSequence s, int start, int before, int count) {}
                @Override public void afterTextChanged(Editable s) {
                    accept.setEnabled(MaiPdfSecurity.isValidPassword(
                        password.getText().toString(), confirmation.getText().toString()));
                }
            };
            password.addTextChangedListener(changed);
            confirmation.addTextChangedListener(changed);
            accept.setOnClickListener(view -> {
                String secret = password.getText().toString();
                if (!request.identity.equals(session.passwordChangeIdentity()) || !request.state.equals("pending")) {
                    cancel(request.id);
                    return;
                }
                if (!MaiPdfSecurity.isValidPassword(secret, confirmation.getText().toString())) return;
                request.password = secret;
                request.state = "ready";
                dialog.dismiss();
            });
        });
        dialog.setOnDismissListener(ignored -> {
            password.setText("");
            confirmation.setText("");
            request.dialog = null;
            if (request.id.equals(activeId)) activeId = null;
            if (request.state.equals("pending")) cancel(request.id);
        });
        dialog.show();
    }

    private static JSONObject state(String value) throws Exception {
        return new JSONObject().put("status", value);
    }

    private void cancel(String id) {
        Request request = requests.get(id);
        if (request == null) return;
        request.password = null;
        request.state = "canceled";
        AlertDialog dialog = request.dialog;
        request.dialog = null;
        if (id.equals(activeId)) activeId = null;
        if (dialog != null) {
            try { dialog.dismiss(); }
            catch (RuntimeException detachedWindow) {
                // 窗口已被系统移除时仍完成本地清理，不把底层 UI 异常或输入内容回灌。
            }
        }
    }

    /** 普通业务 worker 可直接等待此原生表单，不借用 Agent/question 协议。 */
    public String requestPassword() throws Exception {
        if (Looper.myLooper() == Looper.getMainLooper())
            throw new IllegalStateException("Wait for PDF password on a worker thread");
        String id = UUID.randomUUID().toString();
        try {
            execute(new JSONObject().put("action", "begin").put("request_id", id));
            while (true) {
                if (Thread.currentThread().isInterrupted()) throw new InterruptedException();
                JSONObject response = execute(new JSONObject().put("action", "poll").put("request_id", id));
                if (response.optString("status").equals("ready")) return response.getString("password");
                if (response.optString("status").equals("canceled")) return null;
                Thread.sleep(100);
            }
        } finally {
            main.post(() -> cancel(id));
        }
    }

    public void onDestroy() {
        for (String id : requests.keySet()) cancel(id);
    }
}
