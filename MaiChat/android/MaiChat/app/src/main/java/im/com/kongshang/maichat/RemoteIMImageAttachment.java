// 文件作用：Android IM 身份、消息同步与本地历史；RemoteIMImageAttachment 负责消息附件元数据、路径和可用状态，本文件给出其实现。
// 主要入口：localPath、width、height、sizeBytes。
package com.kongshang.maichat;

import java.util.Objects;

public final class RemoteIMImageAttachment {
    private final String localPath;
    private final int width;
    private final int height;
    private final long sizeBytes;

    public RemoteIMImageAttachment(String localPath, int width, int height, long sizeBytes) {
        this.localPath = clean(localPath);
        this.width = width;
        this.height = height;
        this.sizeBytes = sizeBytes;
        if (this.localPath.isEmpty()) {
            throw new IllegalArgumentException("localPath is required");
        }
    }

    public String localPath() {
        return localPath;
    }

    public int width() {
        return width;
    }

    public int height() {
        return height;
    }

    public long sizeBytes() {
        return sizeBytes;
    }

    private static String clean(String value) {
        return value == null ? "" : value.trim();
    }

    @Override
    public boolean equals(Object other) {
        if (this == other) return true;
        if (!(other instanceof RemoteIMImageAttachment)) return false;
        RemoteIMImageAttachment that = (RemoteIMImageAttachment) other;
        return width == that.width
            && height == that.height
            && sizeBytes == that.sizeBytes
            && localPath.equals(that.localPath);
    }

    @Override
    public int hashCode() {
        return Objects.hash(localPath, width, height, sizeBytes);
    }
}
