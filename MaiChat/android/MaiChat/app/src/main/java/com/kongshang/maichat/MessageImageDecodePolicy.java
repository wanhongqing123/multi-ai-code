package com.kongshang.maichat;

/**
 * 消息图片的解码策略：算降采样倍数、拼缓存键。
 *
 * 单独一个类、不碰任何 Android API，是为了能在 JVM 单测里真跑——
 * BitmapFactory / LruCache 在单测里都是会抛异常的桩，混在一起就没法测了。
 */
public final class MessageImageDecodePolicy {
    private MessageImageDecodePolicy() {
    }

    /**
     * BitmapFactory.Options.inSampleSize：只能取 2 的幂，取到「不小于目标尺寸的最小一档」。
     *
     * 不能一路降到刚好等于目标：inSampleSize 是整数倍缩小，取过头会让图糊。
     * 所以循环条件是「再降一档仍然不小于目标」才继续降。
     */
    public static int sampleSize(int sourceWidth, int sourceHeight, int targetWidth, int targetHeight) {
        if (sourceWidth <= 0 || sourceHeight <= 0 || targetWidth <= 0 || targetHeight <= 0) return 1;
        int sample = 1;
        while (sourceWidth / (sample * 2) >= targetWidth && sourceHeight / (sample * 2) >= targetHeight) {
            sample *= 2;
        }
        return sample;
    }

    /**
     * 缓存键 = 路径 + 目标像素尺寸。
     *
     * 尺寸必须进键：气泡缩略图和全屏预览是同一个文件的两个不同解码结果，
     * 共用一个键会让先到的那个把另一个尺寸顶掉，表现为「预览很糊」或「气泡很占内存」。
     */
    public static String cacheKey(String path, int targetWidth, int targetHeight) {
        String cleanPath = path == null ? "" : path.trim();
        return cleanPath + "|" + Math.max(0, targetWidth) + "x" + Math.max(0, targetHeight);
    }
}
