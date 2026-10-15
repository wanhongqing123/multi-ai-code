// 文件作用：确认桌面扫码请求从 pending 进入 scanned 后仍允许用户批准或拒绝。
package com.kongshang.maichat;

import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

public class DesktopRequestInfoTest {
    @Test public void scannedRequestCanStillBeDecided() {
        assertTrue(info("pending", 120).isAwaitingDecision());
        assertTrue(info("scanned", 95).isAwaitingDecision());
    }

    @Test public void terminalAndExpiredRequestsCannotBeDecided() {
        assertFalse(info("scanned", 0).isAwaitingDecision());
        assertFalse(info("approved", 95).isAwaitingDecision());
        assertFalse(info("denied", 95).isAwaitingDecision());
    }

    private static MaiChatAuthClient.DesktopRequestInfo info(String status, int expiresIn) {
        return new MaiChatAuthClient.DesktopRequestInfo("mac", "123456", status, expiresIn);
    }
}
