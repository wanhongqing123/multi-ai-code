// 文件作用：确认旧设备槽和同槽替换的 401 都会终止本地会话。
package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

public class AuthTerminalErrorTest {
    @Test public void passwordChangeRequiresActualBooleanConfirmation() throws Exception {
        org.junit.Assert.assertFalse(MaiChatAuthClient.passwordSetConfirmed(new org.json.JSONObject()));
        org.junit.Assert.assertFalse(MaiChatAuthClient.passwordSetConfirmed(
            new org.json.JSONObject().put("password_set", "true")));
        org.junit.Assert.assertFalse(MaiChatAuthClient.passwordSetConfirmed(
            new org.json.JSONObject().put("password_set", false)));
        assertTrue(MaiChatAuthClient.passwordSetConfirmed(
            new org.json.JSONObject().put("password_set", true)));
    }

    @Test public void deviceSlotRequiredShowsUpgradeMessage() {
        MaiChatAuthClient.SessionTerminatedException error =
            MaiChatAuthClient.terminalSessionError(401, "DEVICE_SLOT_REQUIRED");
        assertTrue(error instanceof MaiChatAuthClient.DeviceSlotRequiredException);
        assertEquals("请更新 App 后重新登录", error.getMessage());
    }

    @Test public void replacedSessionRemainsTerminal() {
        assertTrue(MaiChatAuthClient.terminalSessionError(401, "SESSION_REPLACED")
            instanceof MaiChatAuthClient.SessionReplacedException);
    }

    @Test public void otherResponsesKeepTheirExistingHandling() {
        assertNull(MaiChatAuthClient.terminalSessionError(401, ""));
        assertNull(MaiChatAuthClient.terminalSessionError(403, "DEVICE_SLOT_REQUIRED"));
    }

    @Test public void modelPermissionDenialShowsAccountAccessReason() {
        assertEquals("当前账号未开通模型使用权限",
            MaiChatAuthClient.errorText("Model access is not enabled for this account", "", 403));
    }

    @Test public void unknownFriendAccountHasActionableMessage() {
        assertEquals("未找到这个 MaiChat 账号",
            MaiChatAuthClient.errorText("MaiChat account not found", "", 404));
        assertEquals("查找好友过于频繁，请稍后再试",
            MaiChatAuthClient.errorText("Friend lookup requests are too frequent", "", 429));
    }

    @Test public void accountPasswordAndFriendshipErrorsAreNotShownAsServiceFailures() {
        assertEquals("账号或密码错误",
            MaiChatAuthClient.errorText("Account or password is incorrect", "", 401));
        assertEquals("你们已经是好友",
            MaiChatAuthClient.errorText("Already friends", "", 409));
    }
}
