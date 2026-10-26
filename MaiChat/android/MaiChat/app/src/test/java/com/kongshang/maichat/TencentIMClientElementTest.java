// 文件作用：Android 本地测试；TencentIMClientElementTest 负责覆盖对应类的状态和数据行为，本文件给出其验证。
// 运行边界：仅供自动化验证使用，不作为正式 App 或 Agent 的用户功能入口。
// 覆盖场景：textFirstMultiElementMessageStillRetainsItsImage、attachmentFirstMessageStillFindsFollowingCaption、
// handlerRoutesTextFirstMultiElementMessageToImageDelivery。
package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;

import android.content.Context;

import androidx.test.core.app.ApplicationProvider;

import com.tencent.imsdk.v2.V2TIMDownloadCallback;
import com.tencent.imsdk.v2.V2TIMElem;
import com.tencent.imsdk.v2.V2TIMFileElem;
import com.tencent.imsdk.v2.V2TIMImageElem;
import com.tencent.imsdk.v2.V2TIMMessage;
import com.tencent.imsdk.v2.V2TIMSoundElem;
import com.tencent.imsdk.v2.V2TIMTextElem;
import com.tencent.imsdk.v2.V2TIMValueCallback;
import com.tencent.imsdk.v2.V2TIMVideoElem;

import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;

import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class TencentIMClientElementTest {
    @Test
    public void textFirstMultiElementMessageStillRetainsItsImage() {
        FakeImageElem image = new FakeImageElem();
        FakeTextElem text = new FakeTextElem("先文字后图片", image);
        FakeMessage message = new FakeMessage(text);

        TencentIMClient.IncomingElementParts parts =
            TencentIMClient.incomingElementParts(message);

        assertEquals("先文字后图片", parts.caption);
        assertTrue(parts.image == image);
    }

    @Test
    public void attachmentFirstMessageStillFindsFollowingCaption() {
        FakeTextElem text = new FakeTextElem("图片下面的配文", null);
        FakeImageElem image = new FakeImageElem(text);
        FakeMessage message = new FakeMessage(image);

        TencentIMClient.IncomingElementParts parts =
            TencentIMClient.incomingElementParts(message);

        assertTrue(parts.image == image);
        assertEquals("图片下面的配文", parts.caption);
    }

    @Test
    public void handlerRoutesTextFirstMultiElementMessageToImageDelivery() throws Exception {
        Context context = ApplicationProvider.getApplicationContext();
        List<RemoteIMMessage> received = new ArrayList<>();
        CountDownLatch delivered = new CountDownLatch(1);
        AtomicReference<String> requestedUrl = new AtomicReference<>();
        TencentIMClient client = new TencentIMClient(context, new TencentIMClient.Listener() {
            @Override public void onConnectionStateChanged(
                TencentIMClient.ConnectionState state,
                String detail
            ) {
            }

            @Override public void onIncomingMessage(RemoteIMMessage message) {
                received.add(message);
                delivered.countDown();
            }

            @Override public void onProfilesUpdated(List<RemoteIMContact> contacts) {
            }

            @Override public void onPresenceUpdated(
                Map<String, TencentIMClient.PresenceStatus> statuses
            ) {
            }
        }, (url, target, maximumBytes, scope, completion) -> {
            requestedUrl.set(url);
            assertEquals(MaiChatAttachmentDownload.MAX_IMAGE_BYTES, maximumBytes);
            completion.finished(new MaiChatAttachmentDownload.Result(
                MaiChatAttachmentDownload.OK, 200, 0, 900));
        });
        FakeImageElem image = new FakeImageElem();
        FakeTextElem text = new FakeTextElem("先文字后图片", image);
        FakeMessage message = new FakeMessage(text);

        client.handleIncomingMessage(message);

        assertTrue(delivered.await(3, TimeUnit.SECONDS));
        assertEquals("https://example.test/text-first.png", requestedUrl.get());
        assertEquals(1, received.size());
        assertNotNull(received.get(0).imageAttachment());
        assertEquals("先文字后图片", received.get(0).text());
        assertTrue(received.get(0).captionAbove());
        assertEquals("message-text-first-1", received.get(0).remoteId());
        assertEquals(
            new RemoteIMQuote("quoted-sdk-1", "desktop-peer", "被引用的原文", "text"),
            received.get(0).quote()
        );
        client.destroy();
    }

    @Test
    public void canceledAccountScopeCannotDeliverLateImage() throws Exception {
        Context context = ApplicationProvider.getApplicationContext();
        CountDownLatch received = new CountDownLatch(1);
        AtomicReference<MaiChatAttachmentDownload.Scope> scope = new AtomicReference<>();
        AtomicReference<MaiChatAttachmentDownload.Completion> completion = new AtomicReference<>();
        TencentIMClient client = new TencentIMClient(context, new TencentIMClient.Listener() {
            @Override public void onConnectionStateChanged(
                TencentIMClient.ConnectionState state, String detail) { }
            @Override public void onIncomingMessage(RemoteIMMessage message) { received.countDown(); }
            @Override public void onProfilesUpdated(List<RemoteIMContact> contacts) { }
            @Override public void onPresenceUpdated(
                Map<String, TencentIMClient.PresenceStatus> statuses) { }
        }, (url, target, maximumBytes, requestScope, callback) -> {
            scope.set(requestScope);
            completion.set(callback);
        });
        client.handleIncomingMessage(new FakeMessage(new FakeImageElem()));
        assertNotNull(scope.get());
        assertNotNull(completion.get());
        scope.get().cancel();
        completion.get().finished(new MaiChatAttachmentDownload.Result(
            MaiChatAttachmentDownload.OK, 200, 0, 900));
        assertFalse(received.await(250, TimeUnit.MILLISECONDS));
        client.destroy();
    }

    @Test
    public void videoCanAppearBeforeCoverThenUpdateSameMessage() throws Exception {
        Context context = ApplicationProvider.getApplicationContext();
        CountDownLatch initial = new CountDownLatch(1);
        CountDownLatch coverUpdated = new CountDownLatch(1);
        AtomicReference<RemoteIMVideoAttachment> firstAttachment = new AtomicReference<>();
        AtomicReference<RemoteIMVideoAttachment> updatedAttachment = new AtomicReference<>();
        AtomicReference<MaiChatAttachmentDownload.Completion> coverDownload = new AtomicReference<>();
        TencentIMClient client = new TencentIMClient(context, new TencentIMClient.Listener() {
            @Override public void onConnectionStateChanged(
                TencentIMClient.ConnectionState state, String detail) { }
            @Override public void onIncomingMessage(RemoteIMMessage message) {
                firstAttachment.set(message.videoAttachment());
                initial.countDown();
            }
            @Override public void onVideoMediaUpdated(String sender, String recipient,
                    String remoteId, RemoteIMVideoAttachment attachment) {
                updatedAttachment.set(attachment);
                coverUpdated.countDown();
            }
            @Override public void onProfilesUpdated(List<RemoteIMContact> contacts) { }
            @Override public void onPresenceUpdated(
                Map<String, TencentIMClient.PresenceStatus> statuses) { }
        }, (url, target, maximumBytes, scope, callback) -> {
            if (url.endsWith("/cover")) {
                assertEquals(MaiChatAttachmentDownload.MAX_VIDEO_COVER_BYTES, maximumBytes);
                coverDownload.set(callback);
            } else {
                assertEquals("https://example.test/video", url);
                assertEquals(MaiChatAttachmentDownload.MAX_VIDEO_BYTES, maximumBytes);
                callback.finished(new MaiChatAttachmentDownload.Result(
                    MaiChatAttachmentDownload.OK, 200, 0, 1024));
            }
        });
        client.handleIncomingMessage(new FakeMessage(new FakeVideoElem()));
        assertTrue(initial.await(3, TimeUnit.SECONDS));
        assertNotNull(firstAttachment.get());
        assertFalse(firstAttachment.get().hasCover());
        assertNotNull(coverDownload.get());
        coverDownload.get().finished(new MaiChatAttachmentDownload.Result(
            MaiChatAttachmentDownload.OK, 200, 0, 100));
        assertTrue(coverUpdated.await(3, TimeUnit.SECONDS));
        assertTrue(updatedAttachment.get().hasCover());
        client.destroy();
    }

    @Test
    public void voiceAndFileResolveSignedUrlsThenUseSharedDownloader() throws Exception {
        Context context = ApplicationProvider.getApplicationContext();
        CountDownLatch delivered = new CountDownLatch(2);
        List<RemoteIMMessage> messages = new CopyOnWriteArrayList<>();
        List<String> requested = new CopyOnWriteArrayList<>();
        TencentIMClient client = new TencentIMClient(context, new TencentIMClient.Listener() {
            @Override public void onConnectionStateChanged(
                TencentIMClient.ConnectionState state, String detail) { }
            @Override public void onIncomingMessage(RemoteIMMessage message) {
                messages.add(message);
                delivered.countDown();
            }
            @Override public void onProfilesUpdated(List<RemoteIMContact> contacts) { }
            @Override public void onPresenceUpdated(
                Map<String, TencentIMClient.PresenceStatus> statuses) { }
        }, (url, target, maximumBytes, scope, callback) -> {
            requested.add(url + "#" + maximumBytes);
            callback.finished(new MaiChatAttachmentDownload.Result(
                MaiChatAttachmentDownload.OK, 200, 0, 64));
        });
        client.handleIncomingMessage(new FakeMessage(new FakeSoundElem()));
        client.handleIncomingMessage(new FakeMessage(new FakeFileElem()));
        assertTrue(delivered.await(3, TimeUnit.SECONDS));
        assertEquals(2, messages.size());
        assertNotNull(messages.get(0).voiceAttachment());
        assertNotNull(messages.get(1).fileAttachment());
        assertTrue(requested.contains("https://example.test/voice#"
            + MaiChatAttachmentDownload.MAX_VOICE_BYTES));
        assertTrue(requested.contains("https://example.test/file#"
            + MaiChatAttachmentDownload.MAX_FILE_BYTES));
        client.destroy();
    }

    private static final class FakeMessage extends V2TIMMessage {
        private final V2TIMElem first;

        FakeMessage(V2TIMElem first) {
            this.first = first;
        }

        @Override public int getElemType() {
            return first instanceof V2TIMTextElem
                ? V2TIM_ELEM_TYPE_TEXT
                : first instanceof V2TIMVideoElem ? V2TIM_ELEM_TYPE_VIDEO
                : first instanceof V2TIMSoundElem ? V2TIM_ELEM_TYPE_SOUND
                : first instanceof V2TIMFileElem ? V2TIM_ELEM_TYPE_FILE
                : V2TIM_ELEM_TYPE_IMAGE;
        }

        @Override public V2TIMTextElem getTextElem() {
            return first instanceof V2TIMTextElem ? (V2TIMTextElem) first : null;
        }

        @Override public V2TIMImageElem getImageElem() {
            return first instanceof V2TIMImageElem ? (V2TIMImageElem) first : null;
        }

        @Override public V2TIMVideoElem getVideoElem() {
            return first instanceof V2TIMVideoElem ? (V2TIMVideoElem) first : null;
        }

        @Override public V2TIMSoundElem getSoundElem() {
            return first instanceof V2TIMSoundElem ? (V2TIMSoundElem) first : null;
        }

        @Override public V2TIMFileElem getFileElem() {
            return first instanceof V2TIMFileElem ? (V2TIMFileElem) first : null;
        }

        @Override public boolean isSelf() {
            return false;
        }

        @Override public String getSender() {
            return "desktop-peer";
        }

        @Override public long getTimestamp() {
            return 100L;
        }

        @Override public String getMsgID() {
            return "message-text-first-1";
        }

        @Override public String getCloudCustomData() {
            return "{\"namespace\":\"multi-ai-code\",\"version\":2,"
                + "\"origin\":\"human\",\"captionAbove\":true,"
                + "\"quote\":{\"msgId\":\"quoted-sdk-1\",\"sender\":\"desktop-peer\","
                + "\"digest\":\"被引用的原文\",\"kind\":\"text\"}}";
        }
    }

    private static final class FakeTextElem extends V2TIMTextElem {
        private final String text;
        private final V2TIMElem next;

        FakeTextElem(String text, V2TIMElem next) {
            this.text = text;
            this.next = next;
        }

        @Override public String getText() {
            return text;
        }

        @Override public V2TIMElem getNextElem() {
            return next;
        }
    }

    private static final class FakeImageElem extends V2TIMImageElem {
        private final V2TIMElem next;
        private final List<V2TIMImage> images;

        FakeImageElem() {
            this(null);
        }

        FakeImageElem(V2TIMElem next) {
            this.next = next;
            this.images = List.of(new FakeImage());
        }

        @Override public V2TIMElem getNextElem() {
            return next;
        }

        @Override public List<V2TIMImage> getImageList() {
            return images;
        }

        private final class FakeImage extends V2TIMImageElem.V2TIMImage {
            FakeImage() {
                super();
            }

            @Override public String getUUID() {
                return "image-text-first-1";
            }

            @Override public int getSize() {
                return 900;
            }

            @Override public int getWidth() {
                return 100;
            }

            @Override public int getHeight() {
                return 80;
            }

            @Override public String getUrl() {
                return "https://example.test/text-first.png";
            }

            @Override public void downloadImage(String path, V2TIMDownloadCallback callback) {
                callback.onSuccess();
            }
        }
    }

    private static final class FakeVideoElem extends V2TIMVideoElem {
        @Override public V2TIMElem getNextElem() { return null; }
        @Override public String getVideoUUID() { return "video-one"; }
        @Override public String getVideoPath() { return "video-one.mp4"; }
        @Override public int getVideoSize() { return 1024; }
        @Override public int getDuration() { return 4; }
        @Override public int getSnapshotWidth() { return 320; }
        @Override public int getSnapshotHeight() { return 180; }
        @Override public void getSnapshotUrl(V2TIMValueCallback<String> callback) {
            callback.onSuccess("https://example.test/cover");
        }
        @Override public void getVideoUrl(V2TIMValueCallback<String> callback) {
            callback.onSuccess("https://example.test/video");
        }
    }

    private static final class FakeSoundElem extends V2TIMSoundElem {
        @Override public V2TIMElem getNextElem() { return null; }
        @Override public String getUUID() { return "voice-one"; }
        @Override public int getDuration() { return 2; }
        @Override public void getUrl(V2TIMValueCallback<String> callback) {
            callback.onSuccess("https://example.test/voice");
        }
    }

    private static final class FakeFileElem extends V2TIMFileElem {
        @Override public V2TIMElem getNextElem() { return null; }
        @Override public String getUUID() { return "file-one"; }
        @Override public String getFileName() { return "readme.txt"; }
        @Override public int getFileSize() { return 64; }
        @Override public void getUrl(V2TIMValueCallback<String> callback) {
            callback.onSuccess("https://example.test/file");
        }
    }
}
