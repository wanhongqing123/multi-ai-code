package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertThrows;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

public class RemoteIMVideoAttachmentTest {
    @Test
    public void keepsEveryFieldItWasGiven() {
        RemoteIMVideoAttachment attachment = new RemoteIMVideoAttachment(
            "/tmp/clip.mp4",
            "/tmp/clip-cover.jpg",
            12,
            1080,
            1920,
            4_194_304L
        );

        assertEquals("/tmp/clip.mp4", attachment.localPath());
        assertEquals("/tmp/clip-cover.jpg", attachment.coverPath());
        assertTrue(attachment.hasCover());
        assertEquals(12, attachment.durationSeconds());
        assertEquals(1080, attachment.width());
        assertEquals(1920, attachment.height());
        assertEquals(4_194_304L, attachment.sizeBytes());
    }

    @Test
    public void acceptsMissingCoverBecauseSnapshotIsADownloadOfItsOwn() {
        // 封面和视频是两次独立下载，封面可能失败或还没回来；那不该让整条消息作废。
        RemoteIMVideoAttachment attachment =
            new RemoteIMVideoAttachment("/tmp/clip.mp4", "", 3, 0, 0, 0);

        assertFalse(attachment.hasCover());
        assertEquals("", attachment.coverPath());
    }

    @Test
    public void requiresLocalPath() {
        assertThrows(
            IllegalArgumentException.class,
            () -> new RemoteIMVideoAttachment("   ", "/tmp/cover.jpg", 3, 10, 10, 100)
        );
    }

    @Test
    public void clampsNegativeNumbersInsteadOfPropagatingThem() {
        RemoteIMVideoAttachment attachment =
            new RemoteIMVideoAttachment("/tmp/clip.mp4", null, -5, -1, -1, -20L);

        assertEquals(0, attachment.durationSeconds());
        assertEquals(0, attachment.width());
        assertEquals(0, attachment.height());
        assertEquals(0L, attachment.sizeBytes());
        assertFalse(attachment.hasCover());
    }
}
