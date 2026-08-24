package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotEquals;

import org.junit.Test;

public class MessageImageDecodePolicyTest {
    // 12MP 手机照片贴进 260x190 的气泡：必须降下来，否则主线程要解 48MB 位图。
    @Test
    public void picksASampleSizeThatShrinksAPhonePhotoToBubbleSize() {
        assertEquals(8, MessageImageDecodePolicy.sampleSize(4000, 3000, 260, 190));
    }

    // 不能降过头：inSampleSize 是整数倍缩小，取到小于目标尺寸那一档图会糊。
    @Test
    public void doesNotShrinkBelowTheTargetSize() {
        int sample = MessageImageDecodePolicy.sampleSize(4000, 3000, 260, 190);
        assertEquals("降完仍不小于目标宽", true, 4000 / sample >= 260);
        assertEquals("降完仍不小于目标高", true, 3000 / sample >= 190);
        assertEquals("再降一档就低于目标了", true, 4000 / (sample * 2) < 260 || 3000 / (sample * 2) < 190);
    }

    // 原图本来就比目标小，不该放大也不该降。
    @Test
    public void keepsSmallImagesUntouched() {
        assertEquals(1, MessageImageDecodePolicy.sampleSize(120, 90, 260, 190));
    }

    // 尺寸读不出来（解码失败、文件损坏）时不能返回 0 或负数，那会让 BitmapFactory 抛异常。
    @Test
    public void fallsBackToOneWhenBoundsAreUnknown() {
        assertEquals(1, MessageImageDecodePolicy.sampleSize(0, 0, 260, 190));
        assertEquals(1, MessageImageDecodePolicy.sampleSize(-1, -1, 260, 190));
        assertEquals(1, MessageImageDecodePolicy.sampleSize(4000, 3000, 0, 0));
    }

    // 缓存键必须带目标尺寸：气泡缩略图和全屏预览是同一文件的两个解码结果，
    // 共用一个键会让先到的把另一个顶掉——表现为预览很糊，或者气泡吃掉整屏内存。
    @Test
    public void cacheKeySeparatesDifferentTargetSizes() {
        String bubble = MessageImageDecodePolicy.cacheKey("/data/a.jpg", 260, 190);
        String preview = MessageImageDecodePolicy.cacheKey("/data/a.jpg", 1080, 1920);
        assertNotEquals(bubble, preview);
        assertEquals(bubble, MessageImageDecodePolicy.cacheKey("  /data/a.jpg  ", 260, 190));
    }

    @Test
    public void cacheKeySeparatesDifferentFiles() {
        assertNotEquals(
            MessageImageDecodePolicy.cacheKey("/data/a.jpg", 260, 190),
            MessageImageDecodePolicy.cacheKey("/data/b.jpg", 260, 190));
    }
}
