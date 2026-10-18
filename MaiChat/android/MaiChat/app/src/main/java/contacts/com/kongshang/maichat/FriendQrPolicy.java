// 文件作用：在好友二维码中只编码可分享的 IM 账号 ID，不包含手机号或登录凭据。
package com.kongshang.maichat;

import java.net.URI;

final class FriendQrPolicy {
    private static final String PREFIX = "maichat://friend/add?user_id=";

    private FriendQrPolicy() { }

    static String link(String userId) {
        return valid(userId) ? PREFIX + userId : "";
    }

    static String userId(String text) {
        if (text == null || !text.startsWith(PREFIX)) return "";
        try {
            URI uri = new URI(text);
            if (!"maichat".equalsIgnoreCase(uri.getScheme()) ||
                !"friend".equalsIgnoreCase(uri.getHost()) ||
                !"/add".equals(uri.getPath()) || uri.getFragment() != null ||
                uri.getPort() != -1 || uri.getUserInfo() != null) return "";
            String query = uri.getRawQuery();
            if (query == null || !query.startsWith("user_id=")) return "";
            String userId = query.substring("user_id=".length());
            return valid(userId) && text.equals(PREFIX + userId) ? userId : "";
        } catch (Exception invalid) { return ""; }
    }

    private static boolean valid(String value) {
        return value != null && value.matches("[A-Za-z0-9_-]{1,128}");
    }
}
