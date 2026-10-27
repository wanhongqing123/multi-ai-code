package com.kongshang.maichat;

import android.Manifest;
import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Context;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.net.Uri;
import android.provider.CalendarContract;
import java.io.IOException;
import java.util.TimeZone;
import org.json.JSONObject;

/** Ordinary OS calendar entry used by both Agent tools and app UI. */
final class MaiSystemCalendarEvents {
    private MaiSystemCalendarEvents() {}

    static JSONObject create(Context context, JSONObject input) throws Exception {
        String title = input.optString("title");
        String session = input.optString("session_id");
        String notes = input.optString("notes");
        long start = input.optLong("starts_at_utc_ms");
        long end = input.optLong("ends_at_utc_ms");
        if (title.isEmpty() || title.length() > 120 || session.isEmpty() ||
            end <= start || start <= System.currentTimeMillis() ||
            end - start > 7L * 24 * 60 * 60 * 1000)
            throw new IllegalArgumentException("日历事件或时间无效");
        if (context.checkSelfPermission(Manifest.permission.READ_CALENDAR) !=
                PackageManager.PERMISSION_GRANTED ||
            context.checkSelfPermission(Manifest.permission.WRITE_CALENDAR) !=
                PackageManager.PERMISSION_GRANTED)
            throw new SecurityException("请先允许 MaiChat 读写系统日历");
        ContentResolver resolver = context.getContentResolver();
        long calendarId = -1;
        int bestPrimary = -1;
        String[] projection = {CalendarContract.Calendars._ID,
            CalendarContract.Calendars.CALENDAR_ACCESS_LEVEL,
            CalendarContract.Calendars.IS_PRIMARY};
        try (Cursor calendars = resolver.query(CalendarContract.Calendars.CONTENT_URI,
            projection, null, null, null)) {
            if (calendars != null) while (calendars.moveToNext()) {
                if (calendars.getInt(1) < CalendarContract.Calendars.CAL_ACCESS_CONTRIBUTOR)
                    continue;
                int primary = calendars.getInt(2);
                if (calendarId < 0 || primary > bestPrimary) {
                    calendarId = calendars.getLong(0);
                    bestPrimary = primary;
                }
            }
        }
        if (calendarId < 0) throw new IOException("系统没有可写入的日历");
        ContentValues event = new ContentValues();
        event.put(CalendarContract.Events.CALENDAR_ID, calendarId);
        event.put(CalendarContract.Events.TITLE, title);
        event.put(CalendarContract.Events.DESCRIPTION, notes);
        event.put(CalendarContract.Events.DTSTART, start);
        event.put(CalendarContract.Events.DTEND, end);
        event.put(CalendarContract.Events.EVENT_TIMEZONE, TimeZone.getDefault().getID());
        Uri created = resolver.insert(CalendarContract.Events.CONTENT_URI, event);
        if (created == null || created.getLastPathSegment() == null)
            throw new IOException("系统日历没有确认写入事件");
        long eventId = Long.parseLong(created.getLastPathSegment());
        ContentValues reminder = new ContentValues();
        reminder.put(CalendarContract.Reminders.EVENT_ID, eventId);
        reminder.put(CalendarContract.Reminders.MINUTES, 0);
        reminder.put(CalendarContract.Reminders.METHOD,
            CalendarContract.Reminders.METHOD_ALERT);
        boolean reminderCreated = false;
        try {
            reminderCreated = resolver.insert(CalendarContract.Reminders.CONTENT_URI,
                reminder) != null;
        } catch (RuntimeException ignored) {
            // The event is already committed. Report the missing alarm so a retry
            // cannot silently create a duplicate event.
        }
        return new JSONObject().put("state", "created")
            .put("event_id", created.getLastPathSegment())
            .put("calendar_id", calendarId)
            .put("reminder_created", reminderCreated)
            .put("starts_at_utc_ms", start);
    }
}
