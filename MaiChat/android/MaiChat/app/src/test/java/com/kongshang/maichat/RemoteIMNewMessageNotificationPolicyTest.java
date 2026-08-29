package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

public class RemoteIMNewMessageNotificationPolicyTest {
    @Test
    public void onlySuppressesDuplicateOrVisibleForegroundConversation() {
        assertFalse(RemoteIMNewMessageNotificationPolicy.shouldNotify(false, false, false));
        assertFalse(RemoteIMNewMessageNotificationPolicy.shouldNotify(true, true, true));
        assertTrue(RemoteIMNewMessageNotificationPolicy.shouldNotify(true, true, false));
        assertTrue(RemoteIMNewMessageNotificationPolicy.shouldNotify(true, false, true));
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

        assertEquals("图片消息", RemoteIMNewMessageNotificationPolicy.preview(image));
        assertFalse(RemoteIMNewMessageNotificationPolicy.preview(image).contains("/private"));
        assertEquals("请看这张图 然后回复", RemoteIMNewMessageNotificationPolicy.preview(captioned));
        assertEquals(
            "3 条新消息：请看这张图 然后回复",
            RemoteIMNewMessageNotificationPolicy.aggregatedPreview(captioned, 3)
        );
    }
}
