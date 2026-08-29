package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;

import android.content.Context;

import androidx.test.core.app.ApplicationProvider;

import com.tencent.imsdk.v2.V2TIMDownloadCallback;
import com.tencent.imsdk.v2.V2TIMElem;
import com.tencent.imsdk.v2.V2TIMImageElem;
import com.tencent.imsdk.v2.V2TIMMessage;
import com.tencent.imsdk.v2.V2TIMTextElem;

import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;

import java.util.ArrayList;
import java.util.List;
import java.util.Map;

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
    public void handlerRoutesTextFirstMultiElementMessageToImageDelivery() {
        Context context = ApplicationProvider.getApplicationContext();
        List<RemoteIMMessage> received = new ArrayList<>();
        TencentIMClient client = new TencentIMClient(context, new TencentIMClient.Listener() {
            @Override public void onConnectionStateChanged(
                TencentIMClient.ConnectionState state,
                String detail
            ) {
            }

            @Override public void onIncomingMessage(RemoteIMMessage message) {
                received.add(message);
            }

            @Override public void onProfilesUpdated(List<RemoteIMContact> contacts) {
            }

            @Override public void onPresenceUpdated(
                Map<String, TencentIMClient.PresenceStatus> statuses
            ) {
            }
        });
        FakeImageElem image = new FakeImageElem();
        FakeTextElem text = new FakeTextElem("先文字后图片", image);
        FakeMessage message = new FakeMessage(text);

        client.handleIncomingMessage(message);

        assertEquals(1, received.size());
        assertNotNull(received.get(0).imageAttachment());
        assertEquals("先文字后图片", received.get(0).text());
        assertTrue(received.get(0).captionAbove());
    }

    private static final class FakeMessage extends V2TIMMessage {
        private final V2TIMElem first;

        FakeMessage(V2TIMElem first) {
            this.first = first;
        }

        @Override public int getElemType() {
            return first instanceof V2TIMTextElem
                ? V2TIM_ELEM_TYPE_TEXT
                : V2TIM_ELEM_TYPE_IMAGE;
        }

        @Override public V2TIMTextElem getTextElem() {
            return first instanceof V2TIMTextElem ? (V2TIMTextElem) first : null;
        }

        @Override public V2TIMImageElem getImageElem() {
            return first instanceof V2TIMImageElem ? (V2TIMImageElem) first : null;
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
                + "\"origin\":\"human\",\"captionAbove\":true}";
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
}
