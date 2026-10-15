// 文件作用：Android IM 身份、消息同步与本地历史；RemoteIMSettings 负责用户配置的读取、更新和持久化，本文件给出其实现。
// 主要入口：empty、loginUserId、requiresLogin、equals。
package com.kongshang.maichat;

import java.util.Objects;

public final class RemoteIMSettings {
    private final String loginUserId;

    public RemoteIMSettings(String loginUserId) {
        this.loginUserId = loginUserId == null ? "" : loginUserId.trim();
    }

    public static RemoteIMSettings empty() {
        return new RemoteIMSettings("");
    }

    public String loginUserId() {
        return loginUserId;
    }

    public boolean requiresLogin() {
        return loginUserId.isEmpty();
    }

    @Override
    public boolean equals(Object other) {
        if (this == other) return true;
        if (!(other instanceof RemoteIMSettings)) return false;
        RemoteIMSettings settings = (RemoteIMSettings) other;
        return loginUserId.equals(settings.loginUserId);
    }

    @Override
    public int hashCode() {
        return Objects.hash(loginUserId);
    }
}
