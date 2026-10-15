// 文件作用：Android 本地测试；RemoteIMActivityDurationFormatterTest 负责覆盖对应类的状态和数据行为，本文件给出其验证。
// 运行边界：仅供自动化验证使用，不作为正式 App 或 Agent 的用户功能入口。
// 覆盖场景：formatsSecondsAndMinutesLikeTheOtherClients。
package com.kongshang.maichat;

import org.junit.Test;
import static org.junit.Assert.assertEquals;

public class RemoteIMActivityDurationFormatterTest {
    @Test public void formatsSecondsAndMinutesLikeTheOtherClients() {
        assertEquals("0秒", RemoteIMActivityDurationFormatter.text(-1));
        assertEquals("59秒", RemoteIMActivityDurationFormatter.text(59));
        assertEquals("1分0秒", RemoteIMActivityDurationFormatter.text(60));
        assertEquals("6分57秒", RemoteIMActivityDurationFormatter.text(417));
    }
}
