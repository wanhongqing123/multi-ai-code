package com.kongshang.maichat;

import java.util.Objects;

public final class RemoteIMVideoAttachment {
    private final String localPath;
    private final String coverPath;
    private final int durationSeconds;
    private final int width;
    private final int height;
    private final long sizeBytes;

    public RemoteIMVideoAttachment(
        String localPath,
        String coverPath,
        int durationSeconds,
        int width,
        int height,
        long sizeBytes
    ) {
        this.localPath = clean(localPath);
        // 封面允许为空：snapshot 和 video 是两次独立下载，封面可能先到、也可能失败，
        // 这两种情况都不该让整条消息作废。
        this.coverPath = clean(coverPath);
        this.durationSeconds = Math.max(0, durationSeconds);
        this.width = Math.max(0, width);
        this.height = Math.max(0, height);
        this.sizeBytes = Math.max(0, sizeBytes);
        if (this.localPath.isEmpty()) {
            throw new IllegalArgumentException("localPath is required");
        }
    }

    public String localPath() {
        return localPath;
    }

    public String coverPath() {
        return coverPath;
    }

    public boolean hasCover() {
        return !coverPath.isEmpty();
    }

    public int durationSeconds() {
        return durationSeconds;
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
        if (!(other instanceof RemoteIMVideoAttachment)) return false;
        RemoteIMVideoAttachment that = (RemoteIMVideoAttachment) other;
        return durationSeconds == that.durationSeconds
            && width == that.width
            && height == that.height
            && sizeBytes == that.sizeBytes
            && localPath.equals(that.localPath)
            && coverPath.equals(that.coverPath);
    }

    @Override
    public int hashCode() {
        return Objects.hash(localPath, coverPath, durationSeconds, width, height, sizeBytes);
    }
}
