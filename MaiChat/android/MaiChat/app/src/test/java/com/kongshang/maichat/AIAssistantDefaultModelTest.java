// 文件作用：验证创作中心首次模型选择与旧会话显式选择的边界。
package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;

import org.junit.Test;

public final class AIAssistantDefaultModelTest {
    @Test public void freshOrInvalidSettingsSelectDeepseekWithoutOverwritingExplicitChoice() {
        assertEquals("deepseek-flash", AIAssistantController.supportedModelOrDefault(""));
        assertEquals("deepseek-flash", AIAssistantController.supportedModelOrDefault("legacy"));
        assertEquals("glm-5.3", AIAssistantController.supportedModelOrDefault("glm-5.3"));
        assertEquals("glm-5.3-flash",
            AIAssistantController.supportedModelOrDefault("glm-5.3-flash"));
        assertEquals("deepseek-flash",
            AIAssistantController.supportedModelOrDefault("deepseek-flash"));
    }
}
