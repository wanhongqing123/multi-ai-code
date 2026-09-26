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
