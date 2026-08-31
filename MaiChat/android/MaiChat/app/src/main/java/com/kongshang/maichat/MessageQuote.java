package com.kongshang.maichat;

import java.io.File;

public final class MessageQuote {
    static final int DIGEST_LIMIT = 120;

    private MessageQuote() {
    }

    public static RemoteIMQuote from(RemoteIMMessage message) {
        if (message == null) throw new IllegalArgumentException("message is required");
        return new RemoteIMQuote(
            message.remoteId(),
            message.fromUserId(),
            digest(message),
            kind(message)
        );
    }

    public static String kind(RemoteIMMessage message) {
        if (message.imageAttachment() != null) return "image";
        if (message.videoAttachment() != null) return "video";
        if (message.voiceAttachment() != null) return "voice";
        if (message.fileAttachment() != null) return "file";
        return "text";
    }

    public static String digest(RemoteIMMessage message) {
        String caption = authoredCaption(message.text());
        if (message.imageAttachment() != null) return clamp(caption.isEmpty() ? "[图片]" : caption);
        if (message.videoAttachment() != null) return clamp(caption.isEmpty() ? "[视频]" : caption);
        if (message.voiceAttachment() != null) return clamp(caption.isEmpty() ? "[语音]" : caption);
        if (message.fileAttachment() != null) {
            if (!caption.isEmpty()) return clamp(caption);
            String rawName = message.fileAttachment().fileName();
            if (rawName == null || rawName.trim().isEmpty()) rawName = message.fileAttachment().localPath();
            String name = basename(rawName);
            return clamp(name.isEmpty() ? "[文件]" : "[文件] " + name);
        }
        return clamp(caption);
    }

    private static String authoredCaption(String text) {
        String clean = clean(text);
        if (clean.startsWith("[图片消息] ")
            || clean.startsWith("[文件消息] ")
            || clean.startsWith("[视频消息] ")
            || clean.startsWith("[语音消息] ")) return "";
        return clean;
    }

    private static String basename(String value) {
        String normalized = clean(value).replace('\\', '/');
        int slash = normalized.lastIndexOf('/');
        String name = slash >= 0 ? normalized.substring(slash + 1) : normalized;
        return new File(name).getName().trim();
    }

    private static String clamp(String value) {
        String clean = clean(value).replaceAll("\\s+", " ");
        if (clean.length() <= DIGEST_LIMIT) return clean;
        return clean.substring(0, DIGEST_LIMIT) + "…";
    }

    private static String clean(String value) {
        return value == null ? "" : value.trim();
    }
}
