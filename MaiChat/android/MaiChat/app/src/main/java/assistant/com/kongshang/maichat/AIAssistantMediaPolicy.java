// 文件作用：Android 创作中心的会话、消息和 Agent 交互；AIAssistantMediaPolicy 负责输入筛选、权限判断或展示规则，本文件给出其实现。
// 主要入口：supportsImages、isSupportedImageMime、maximumBytes、looksLikeImageName。
package com.kongshang.maichat;

/** Pure policy shared by the picker, importer and model guard. */
final class AIAssistantMediaPolicy {
    private static final int MAXIMUM_IMAGE_BYTES = 20 * 1024 * 1024;
    private static final int MAXIMUM_TEXT_BYTES = 5 * 1024 * 1024;

    private AIAssistantMediaPolicy() { }

    static boolean supportsImages(String model) {
        return model != null && !model.equalsIgnoreCase("glm-5.3");
    }

    static boolean isSupportedImageMime(String mimeType) {
        if (mimeType == null) return false;
        String value = mimeType.toLowerCase(java.util.Locale.ROOT);
        return value.equals("image/jpeg") || value.equals("image/png")
            || value.equals("image/webp") || value.equals("image/gif");
    }

    static int maximumBytes(boolean image) {
        return image ? MAXIMUM_IMAGE_BYTES : MAXIMUM_TEXT_BYTES;
    }

    static boolean looksLikeImageName(String name) {
        if (name == null) return false;
        String value = name.toLowerCase(java.util.Locale.ROOT);
        return value.endsWith(".jpg") || value.endsWith(".jpeg") || value.endsWith(".png")
            || value.endsWith(".webp") || value.endsWith(".gif") || value.endsWith(".heic")
            || value.endsWith(".heif");
    }

    static String extension(String mimeType) {
        if (mimeType == null) return "bin";
        switch (mimeType.toLowerCase(java.util.Locale.ROOT)) {
        case "image/png": return "png";
        case "image/jpeg": return "jpg";
        case "image/webp": return "webp";
        case "image/gif": return "gif";
        default: return "bin";
        }
    }
}
