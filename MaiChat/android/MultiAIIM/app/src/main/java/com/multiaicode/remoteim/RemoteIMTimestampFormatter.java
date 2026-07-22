package com.multiaicode.remoteim;

import java.text.SimpleDateFormat;
import java.util.Calendar;
import java.util.Date;
import java.util.Locale;
import java.util.TimeZone;

final class RemoteIMTimestampFormatter {
    private RemoteIMTimestampFormatter() {
    }

    static String format(long createdAtMillis) {
        return format(createdAtMillis, System.currentTimeMillis(), TimeZone.getDefault());
    }

    static String format(long createdAtMillis, long nowMillis, TimeZone timeZone) {
        Calendar messageDate = Calendar.getInstance(timeZone, Locale.CHINA);
        messageDate.setTimeInMillis(createdAtMillis);
        Calendar today = Calendar.getInstance(timeZone, Locale.CHINA);
        today.setTimeInMillis(nowMillis);

        if (isSameDay(messageDate, today)) {
            return formatted(createdAtMillis, "HH:mm", timeZone);
        }

        Calendar yesterday = (Calendar) today.clone();
        yesterday.add(Calendar.DAY_OF_YEAR, -1);
        if (isSameDay(messageDate, yesterday)) {
            return "昨天 " + formatted(createdAtMillis, "HH:mm", timeZone);
        }

        return formatted(createdAtMillis, "M 月 d 日 HH:mm", timeZone);
    }

    private static boolean isSameDay(Calendar left, Calendar right) {
        return left.get(Calendar.ERA) == right.get(Calendar.ERA)
            && left.get(Calendar.YEAR) == right.get(Calendar.YEAR)
            && left.get(Calendar.DAY_OF_YEAR) == right.get(Calendar.DAY_OF_YEAR);
    }

    private static String formatted(long millis, String pattern, TimeZone timeZone) {
        SimpleDateFormat formatter = new SimpleDateFormat(pattern, Locale.CHINA);
        formatter.setTimeZone(timeZone);
        return formatter.format(new Date(millis));
    }
}
