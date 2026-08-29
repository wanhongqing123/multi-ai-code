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
