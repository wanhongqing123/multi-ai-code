package com.kongshang.maichat;

public final class RemoteIMNewMessageNotificationPolicy {
    private RemoteIMNewMessageNotificationPolicy() {
    }

    public static boolean shouldNotify(
        boolean wasInserted,
        boolean activityInForeground,
        boolean conversationVisible
    ) {
        return wasInserted && !(activityInForeground && conversationVisible);
    }

    public static String preview(RemoteIMMessage message) {
        return preview(message, 80);
    }

    public static String aggregatedPreview(RemoteIMMessage message, int pendingCount) {
        String latest = preview(message);
        return pendingCount <= 1 ? latest : pendingCount + " 条新消息：" + latest;
    }

    static String preview(RemoteIMMessage message, int limit) {
        if (message == null) return "新消息";
        String text = compact(message.text());
        String value;
        if (message.imageAttachment() != null) {
            value = text.isEmpty() || text.startsWith("[图片消息]") ? "图片消息" : text;
        } else if (message.fileAttachment() != null) {
            value = text.isEmpty() || text.startsWith("[文件消息]")
                ? "文件：" + message.fileAttachment().fileName()
                : text;
        } else if (message.videoAttachment() != null) {
            value = text.isEmpty() || text.startsWith("[视频消息") ? "视频消息" : text;
        } else if (message.voiceAttachment() != null) {
            value = "语音消息（" + message.voiceAttachment().durationSeconds() + " 秒）";
        } else {
            value = text.isEmpty() ? "新消息" : text;
        }
        int safeLimit = Math.max(1, limit);
        return value.length() > safeLimit ? value.substring(0, safeLimit) + "…" : value;
    }

    private static String compact(String value) {
        return value == null ? "" : value.trim().replaceAll("\\s+", " ");
    }
}
