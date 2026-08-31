package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;

import org.junit.Test;

public class MessageQuoteTest {
    @Test
    public void buildsPrivacySafeDigestSnapshotsForEveryMessageKind() {
        RemoteIMMessage image = message("[图片消息] /private/user/photo.jpg");
        image = withImage(image);
        assertEquals("image", MessageQuote.kind(image));
        assertEquals("[图片]", MessageQuote.digest(image));

        RemoteIMMessage file = new RemoteIMMessage(
            "file-1", "sdk-file-1", "alice", "owner", "[文件消息] secret.xlsx",
            RemoteIMMessage.Direction.INCOMING, RemoteIMMessage.Status.RECEIVED, 1,
            null, null,
            new RemoteIMFileAttachment("/private/payroll/secret.xlsx", "C:\\薪资\\2026年薪资表.xlsx", "application/xlsx", 10),
            null, RemoteIMOrigin.HUMAN
        );
        assertEquals("[文件] 2026年薪资表.xlsx", MessageQuote.digest(file));
    }

    @Test
    public void collapsesWhitespaceAndTruncatesLongText() {
        String longText = "第一行\n\t" + "很长".repeat(80);
        String digest = MessageQuote.digest(message(longText));
        assertEquals(MessageQuote.DIGEST_LIMIT + 1, digest.length());
        assertEquals(true, digest.endsWith("…"));
        assertEquals(false, digest.contains("\n"));
    }

    private static RemoteIMMessage message(String text) {
        return new RemoteIMMessage(
            "text-1", "sdk-text-1", "alice", "owner", text,
            RemoteIMMessage.Direction.INCOMING, RemoteIMMessage.Status.RECEIVED, 1,
            null, null, null, null, RemoteIMOrigin.HUMAN
        );
    }

    private static RemoteIMMessage withImage(RemoteIMMessage source) {
        return new RemoteIMMessage(
            source.id(), source.remoteId(), source.fromUserId(), source.toUserId(), source.text(),
            source.direction(), source.status(), source.createdAtMillis(),
            new RemoteIMImageAttachment("/private/user/photo.jpg", 100, 100, 10),
            null, null, null, source.origin()
        );
    }
}
