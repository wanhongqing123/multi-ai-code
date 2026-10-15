// 文件作用：验证扫码入口只接受无凭据的 MaiChat 桌面登录请求 URI。
package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;

import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 29)
public class DesktopLoginQrUriTest {
    private static final String ID = "123e4567-e89b-12d3-a456-426614174000";
    private static final String URI = "maichat://auth/desktop?request_id=" + ID;

    @Test public void acceptsOnlyTheRequestIdentifier() {
        assertEquals(ID, DesktopLoginQrUri.requestId(URI));
    }

    @Test public void rejectsCredentialsAndAmbiguousQrCodes() {
        assertEquals("", DesktopLoginQrUri.requestId(URI + "&token=secret"));
        assertEquals("", DesktopLoginQrUri.requestId(URI + "&request_id=" + ID));
        assertEquals("", DesktopLoginQrUri.requestId("https://auth/desktop?request_id=" + ID));
        assertEquals("", DesktopLoginQrUri.requestId(
            "maichat://auth/desktop/extra?request_id=" + ID));
        assertEquals("", DesktopLoginQrUri.requestId(
            "maichat://auth/desktop?request_id=not-a-uuid"));
    }
}
