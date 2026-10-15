// 文件作用：Android 本地测试；RemoteDesktopProtocolTest 负责覆盖对应类的状态和数据行为，本文件给出其验证。
// 运行边界：仅供自动化验证使用，不作为正式 App 或 Agent 的用户功能入口。
// 覆盖场景：signalRoundTripsUsingIosCompatibleFieldNames、inputPacketUsesCompactIosProtocolAndFitsTrtcLimit。
package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;

import org.json.JSONObject;
import org.junit.Test;

import java.nio.charset.StandardCharsets;

public class RemoteDesktopProtocolTest {
    @Test
    public void signalRoundTripsUsingIosCompatibleFieldNames() {
        RemoteDesktopSignal original = RemoteDesktopSignal.create(
            RemoteDesktopSignal.Kind.INVITE,
            "session-1",
            "mc-android-room"
        );

        RemoteDesktopSignal decoded = RemoteDesktopSignal.decode(original.encode());

        assertNotNull(decoded);
        assertEquals(RemoteDesktopSignal.Kind.INVITE, decoded.kind);
        assertEquals("session-1", decoded.sessionId);
        assertEquals("mc-android-room", decoded.roomId);
    }

    @Test
    public void inputPacketUsesCompactIosProtocolAndFitsTrtcLimit() throws Exception {
        byte[] data = RemoteInputPacket.encode(
            "session-1",
            7,
            RemoteInputPacket.move(0.25, 0.75),
            RemoteInputPacket.text("你好")
        );
        JSONObject payload = new JSONObject(new String(data, StandardCharsets.UTF_8));

        assertEquals(1, payload.getInt("v"));
        assertEquals("session-1", payload.getString("s"));
        assertEquals(7, payload.getLong("n"));
        assertEquals("m", payload.getJSONArray("e").getJSONObject(0).getString("t"));
        assertEquals("x", payload.getJSONArray("e").getJSONObject(1).getString("t"));
        assertTrue(data.length <= RemoteInputPacket.MAXIMUM_PACKET_BYTES);
    }
}
