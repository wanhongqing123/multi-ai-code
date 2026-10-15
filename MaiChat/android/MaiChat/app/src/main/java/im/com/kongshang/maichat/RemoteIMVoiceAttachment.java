// 文件作用：Android IM 身份、消息同步与本地历史；RemoteIMVoiceAttachment 负责消息附件元数据、路径和可用状态，本文件给出其实现。
// 主要入口：localPath、durationSeconds、clean、equals。
package com.kongshang.maichat;

import java.util.Objects;

public final class RemoteIMVoiceAttachment {
    private final String localPath;
    private final int durationSeconds;

    public RemoteIMVoiceAttachment(String localPath, int durationSeconds) {
        this.localPath = clean(localPath);
        this.durationSeconds = Math.max(0, durationSeconds);
        if (this.localPath.isEmpty()) {
            throw new IllegalArgumentException("localPath is required");
        }
    }

    public String localPath() {
        return localPath;
    }

    public int durationSeconds() {
        return durationSeconds;
    }

    private static String clean(String value) {
        return value == null ? "" : value.trim();
    }

    @Override
    public boolean equals(Object other) {
        if (this == other) return true;
        if (!(other instanceof RemoteIMVoiceAttachment)) return false;
        RemoteIMVoiceAttachment that = (RemoteIMVoiceAttachment) other;
        return durationSeconds == that.durationSeconds && localPath.equals(that.localPath);
    }

    @Override
    public int hashCode() {
        return Objects.hash(localPath, durationSeconds);
    }
}
