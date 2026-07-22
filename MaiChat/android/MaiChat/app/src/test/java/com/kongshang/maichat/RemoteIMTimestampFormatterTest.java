package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;

import org.junit.Test;

import java.util.Calendar;
import java.util.TimeZone;

public class RemoteIMTimestampFormatterTest {
    @Test
    public void formatsTimestampWithDesktopDateRules() {
        TimeZone timeZone = TimeZone.getTimeZone("GMT+08:00");
        long now = millis(timeZone, 2026, Calendar.JULY, 6, 10, 0);
        long today = millis(timeZone, 2026, Calendar.JULY, 6, 16, 13);
        long yesterday = millis(timeZone, 2026, Calendar.JULY, 5, 23, 53);
        long older = millis(timeZone, 2026, Calendar.JULY, 4, 14, 18);

        assertEquals("16:13", RemoteIMTimestampFormatter.format(today, now, timeZone));
        assertEquals("昨天 23:53", RemoteIMTimestampFormatter.format(yesterday, now, timeZone));
        assertEquals("7 月 4 日 14:18", RemoteIMTimestampFormatter.format(older, now, timeZone));
    }

    private static long millis(
        TimeZone timeZone,
        int year,
        int month,
        int day,
        int hour,
        int minute
    ) {
        Calendar calendar = Calendar.getInstance(timeZone);
        calendar.clear();
        calendar.set(year, month, day, hour, minute, 0);
        return calendar.getTimeInMillis();
    }
}
