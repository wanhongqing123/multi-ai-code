// 文件作用：Android 本地测试；AIAssistantMediaPolicyTest 负责覆盖对应类的状态和数据行为，本文件给出其验证。
// 运行边界：仅供自动化验证使用，不作为正式 App 或 Agent 的用户功能入口。
// 覆盖场景：recognizesVisualModelsAndSupportedImageTypes、choosesStableExtensionsAndLimits。
package com.kongshang.maichat;

import org.junit.Test;
import static org.junit.Assert.*;

public class AIAssistantMediaPolicyTest {
    @Test public void recognizesVisualModelsAndSupportedImageTypes() {
        assertFalse(AIAssistantMediaPolicy.supportsImages("glm-5.3"));
        assertTrue(AIAssistantMediaPolicy.supportsImages("glm-5.3-flash"));
        assertTrue(AIAssistantMediaPolicy.isSupportedImageMime("image/png"));
        assertTrue(AIAssistantMediaPolicy.isSupportedImageMime("image/jpeg"));
        assertTrue(AIAssistantMediaPolicy.isSupportedImageMime("image/webp"));
        assertTrue(AIAssistantMediaPolicy.isSupportedImageMime("image/gif"));
        assertFalse(AIAssistantMediaPolicy.isSupportedImageMime("image/heic"));
        assertTrue(AIAssistantMediaPolicy.looksLikeImageName("camera.HEIC"));
        assertFalse(AIAssistantMediaPolicy.looksLikeImageName("notes.txt"));
    }

    @Test public void choosesStableExtensionsAndLimits() {
        assertEquals("png", AIAssistantMediaPolicy.extension("image/png"));
        assertEquals("jpg", AIAssistantMediaPolicy.extension("image/jpeg"));
        assertEquals(20 * 1024 * 1024, AIAssistantMediaPolicy.maximumBytes(true));
        assertEquals(5 * 1024 * 1024, AIAssistantMediaPolicy.maximumBytes(false));
    }
}
