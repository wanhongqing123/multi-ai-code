package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

import com.tencent.imsdk.v2.V2TIMElem;
import com.tencent.imsdk.v2.V2TIMImageElem;
import com.tencent.imsdk.v2.V2TIMMessage;
import com.tencent.imsdk.v2.V2TIMTextElem;

import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;

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

        FakeImageElem() {
            this(null);
        }

        FakeImageElem(V2TIMElem next) {
            this.next = next;
        }

        @Override public V2TIMElem getNextElem() {
            return next;
        }
    }
}
