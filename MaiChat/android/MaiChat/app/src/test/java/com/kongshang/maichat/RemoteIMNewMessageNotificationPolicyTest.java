package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

public class RemoteIMNewMessageNotificationPolicyTest {
    @Test
    public void onlyNotifiesForFirstInsertionWhileInBackground() {
        assertFalse(RemoteIMNewMessageNotificationPolicy.shouldNotify(false, false));
        assertFalse(RemoteIMNewMessageNotificationPolicy.shouldNotify(true, true));
        assertTrue(RemoteIMNewMessageNotificationPolicy.shouldNotify(true, false));
    }

    @Test
    public void attachmentPreviewDoesNotExposeLocalPath() {
        RemoteIMMessage image = new RemoteIMMessage(
            "peer", "android", "[图片消息] private.png",
            RemoteIMMessage.Direction.INCOMING,
            RemoteIMMessage.Status.RECEIVED,
            1,
            new RemoteIMImageAttachment("/private/token/private.png", 100, 80, 900),
            null
        );
        RemoteIMMessage captioned = new RemoteIMMessage(
            "peer", "android", "  请看这张图\n然后回复  ",
            RemoteIMMessage.Direction.INCOMING,
            RemoteIMMessage.Status.RECEIVED,
            2,
            new RemoteIMImageAttachment("/tmp/photo.png", 100, 80, 900),
            null
        );
        RemoteIMMessage sensitiveFile = new RemoteIMMessage(
            "peer", "android", "[文件消息] 2026年薪资表.xlsx",
            RemoteIMMessage.Direction.INCOMING,
            RemoteIMMessage.Status.RECEIVED,
            3,
            null,
            null,
            new RemoteIMFileAttachment(
                "/private/hr/2026年薪资表.xlsx",
                "2026年薪资表.xlsx",
                "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
                1024
            )
        );

        assertEquals("图片消息", RemoteIMNewMessageNotificationPolicy.preview(image));
        assertFalse(RemoteIMNewMessageNotificationPolicy.preview(image).contains("/private"));
        assertEquals("请看这张图 然后回复", RemoteIMNewMessageNotificationPolicy.preview(captioned));
        assertEquals("文件消息", RemoteIMNewMessageNotificationPolicy.preview(sensitiveFile));
        assertFalse(RemoteIMNewMessageNotificationPolicy.preview(sensitiveFile).contains("薪资"));
        assertEquals(
            "3 条新消息：请看这张图 然后回复",
            RemoteIMNewMessageNotificationPolicy.aggregatedPreview(captioned, 3)
        );
    }

    @Test
    public void longTextIsTruncatedForLockScreenPrivacy() {
        String longText = "x".repeat(200);
        RemoteIMMessage message = new RemoteIMMessage(
            "peer", "android", longText,
            RemoteIMMessage.Direction.INCOMING,
            RemoteIMMessage.Status.RECEIVED,
            4,
            null,
            null
        );

        String preview = RemoteIMNewMessageNotificationPolicy.preview(message);
        assertEquals(81, preview.length());
        assertTrue(preview.endsWith("…"));
    }
}
