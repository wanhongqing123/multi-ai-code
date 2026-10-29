// 文件作用：保存本机密码输入页的无凭据请求状态，供普通 UI 和 Agent 查询共用。
// 密码只存在安全输入控件和普通鉴权请求；此类不得接收、保存或返回密码及请求体。
package com.kongshang.maichat;

import java.util.LinkedHashMap;
import java.nio.charset.StandardCharsets;
import java.util.UUID;
import org.json.JSONException;
import org.json.JSONObject;

/** UI 线程专用。请求绑定账号代际，切换账号后不得查询或完成旧账号请求。 */
public final class MaiAccountPasswordRequests {
    private static final class Request {
        final String id = UUID.randomUUID().toString();
        final String identity;
        String status = "opening";
        boolean opened;
        String errorCode;
        Request(String identity) { this.identity = identity; }
    }
    private final LinkedHashMap<String, Request> requests = new LinkedHashMap<>();
    private String activeId;

    public String open(String identity) {
        if (identity == null || identity.isEmpty()) throw new IllegalStateException("请先登录 MaiChat");
        Request active = requests.get(activeId);
        if (active != null && active.identity.equals(identity)) return active.id;
        cancelActive("identity_changed");
        Request request = new Request(identity);
        requests.put(request.id, request);
        activeId = request.id;
        while (requests.size() > 20) requests.remove(requests.keySet().iterator().next());
        return request.id;
    }

    public JSONObject status(String id, String identity) throws JSONException {
        Request request = find(id, identity);
        JSONObject result = new JSONObject().put("request_id", request.id)
            .put("opened", request.opened).put("status", request.status)
            .put("changed", "changed".equals(request.status));
        if (request.errorCode != null) result.put("error_code", request.errorCode);
        return result;
    }

    public boolean matches(String id, String identity) {
        Request request = requests.get(id);
        return request != null && request.identity.equals(identity);
    }

    public void visible(String id, String identity) {
        Request request = find(id, identity);
        // Dialog.show() 的请求不算已打开；onShow 后才允许等待用户输入的状态。
        request.opened = true;
        request.status = "awaiting_user";
        request.errorCode = null;
    }

    public void finished(String id, String identity, String errorCode) {
        Request request = find(id, identity);
        if (!request.opened || !request.status.equals("submitting"))
            throw new IllegalStateException("只有在途请求可以确认修改结果");
        // 调用者只能在普通 API 确认 password_set=true 后传 nil；HTTP 受理不能算修改成功。
        request.status = errorCode == null ? "changed" :
            "reauth_required".equals(errorCode) ? "reauth_required" : "failed";
        request.errorCode = errorCode;
    }

    public void submitting(String id, String identity) {
        Request request = find(id, identity);
        if (!request.opened || request.status.equals("changed") || request.status.equals("submitting") ||
            request.status.equals("canceled"))
            throw new IllegalStateException("密码页当前不能提交");
        request.status = "submitting";
        request.errorCode = null;
    }

    public static boolean isValidPassword(String password, String confirmation) {
        if (password == null || !password.equals(confirmation)) return false;
        int length = password.codePointCount(0, password.length());
        return length >= 15 && length <= 128 && password.getBytes(StandardCharsets.UTF_8).length <= 512;
    }

    public void dismiss(String id) {
        if (!id.equals(activeId)) return;
        Request request = requests.get(id);
        // 网络提交不能由关闭窗口撤回，不能把在途请求误记为已取消。
        if (request != null && request.status.equals("submitting")) return;
        cancelActive("canceled");
    }

    public void cancelActive(String reason) {
        Request request = requests.get(activeId);
        if (request != null && (request.status.equals("opening") || request.status.equals("awaiting_user"))) {
            request.status = "canceled";
            request.errorCode = reason;
        } else if (request != null && request.status.equals("submitting")) {
            request.status = "failed";
            request.errorCode = "identity_changed";
        }
        activeId = null;
    }

    private Request find(String id, String identity) {
        Request request = requests.get(id);
        if (request == null || !request.identity.equals(identity))
            throw new IllegalArgumentException("当前账号没有这条密码修改请求");
        return request;
    }
}
