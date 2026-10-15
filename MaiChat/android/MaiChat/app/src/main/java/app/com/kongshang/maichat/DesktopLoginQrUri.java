// 文件作用：只从 MaiChat 桌面登录二维码中提取无凭据的请求 ID。
package com.kongshang.maichat;

import android.net.Uri;

import java.util.Set;

final class DesktopLoginQrUri {
    private DesktopLoginQrUri() { }

    static String requestId(String raw) {
        if (raw == null || raw.length() > 256) return "";
        try {
            Uri uri = Uri.parse(raw);
            if (!"maichat".equals(uri.getScheme()) || !"auth".equals(uri.getHost())
                || !"/desktop".equals(uri.getEncodedPath()) || uri.getPort() != -1
                || uri.getUserInfo() != null || uri.getFragment() != null) return "";
            Set<String> names = uri.getQueryParameterNames();
            if (names.size() != 1 || !names.contains("request_id")
                || uri.getQueryParameters("request_id").size() != 1) return "";
            String id = uri.getQueryParameter("request_id");
            return id != null && id.matches(
                "[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}")
                ? id : "";
        } catch (RuntimeException ignored) {
            return "";
        }
    }
}
