// 文件作用：Android 本地测试；RemoteIMAttachmentCaptionPolicyTest 负责覆盖对应类的状态和数据行为，本文件给出其验证。
// 运行边界：仅供自动化验证使用，不作为正式 App 或 Agent 的用户功能入口。
// 覆盖场景：placesRealCaptionOnMetadataSelectedSide、legacyAttachmentPlaceholderIsNotRenderedAsCaption、imageMessage。
package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;

import org.junit.Test;

public class RemoteIMAttachmentCaptionPolicyTest {
    @Test
    public void placesRealCaptionOnMetadataSelectedSide() {
        RemoteIMMessage message = imageMessage("先看这句说明");
        message.setCaptionAbove(true);
        assertEquals("先看这句说明", RemoteIMAttachmentCaptionPolicy.caption(message));
        assertEquals(
            RemoteIMAttachmentCaptionPolicy.Placement.ABOVE,
            RemoteIMAttachmentCaptionPolicy.placement(message)
        );

        message.setCaptionAbove(false);
        assertEquals(
            RemoteIMAttachmentCaptionPolicy.Placement.BELOW,
            RemoteIMAttachmentCaptionPolicy.placement(message)
        );
    }

    @Test
    public void legacyAttachmentPlaceholderIsNotRenderedAsCaption() {
        RemoteIMMessage message = imageMessage("[图片消息] photo.png");
        message.setCaptionAbove(true);
        assertEquals("", RemoteIMAttachmentCaptionPolicy.caption(message));
        assertEquals(
            RemoteIMAttachmentCaptionPolicy.Placement.NONE,
            RemoteIMAttachmentCaptionPolicy.placement(message)
        );
    }

    private static RemoteIMMessage imageMessage(String text) {
        return new RemoteIMMessage(
            "mac", "android", text,
            RemoteIMMessage.Direction.INCOMING,
            RemoteIMMessage.Status.RECEIVED,
            100L,
            new RemoteIMImageAttachment("/tmp/photo.png", 100, 80, 900),
            null
        );
    }
}
