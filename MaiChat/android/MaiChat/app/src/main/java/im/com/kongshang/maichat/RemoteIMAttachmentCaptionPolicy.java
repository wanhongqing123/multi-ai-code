// 文件作用：Android IM 身份、消息同步与本地历史；RemoteIMAttachmentCaptionPolicy 负责输入筛选、权限判断或展示规则，本文件给出其实现。
// 主要入口：caption、placement、hasAttachment。
package com.kongshang.maichat;

/** Pure display policy shared by the Activity and JVM tests. */
public final class RemoteIMAttachmentCaptionPolicy {
    public enum Placement {
        NONE,
        ABOVE,
        BELOW
    }

    private RemoteIMAttachmentCaptionPolicy() {
    }

    public static String caption(RemoteIMMessage message) {
        if (message == null || !hasAttachment(message)) return "";
        String text = message.text() == null ? "" : message.text().trim();
        if (text.isEmpty()
            || text.startsWith("[图片消息]")
            || text.startsWith("[文件消息]")
            || text.startsWith("[视频消息")
            || text.startsWith("[语音消息")) {
            return "";
        }
        return text;
    }

    public static Placement placement(RemoteIMMessage message) {
        if (caption(message).isEmpty()) return Placement.NONE;
        return message.captionAbove() ? Placement.ABOVE : Placement.BELOW;
    }

    private static boolean hasAttachment(RemoteIMMessage message) {
        return message.imageAttachment() != null
            || message.voiceAttachment() != null
            || message.videoAttachment() != null
            || message.fileAttachment() != null;
    }
}
