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
