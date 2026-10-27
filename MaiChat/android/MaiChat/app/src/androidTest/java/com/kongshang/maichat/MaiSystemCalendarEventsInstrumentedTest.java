package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;

import android.Manifest;
import android.content.ContentResolver;
import android.content.ContentUris;
import android.content.ContentValues;
import android.content.Context;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.graphics.Color;
import android.net.Uri;
import android.provider.CalendarContract;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import java.util.UUID;
import org.json.JSONObject;
import org.junit.Assume;
import org.junit.Test;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
public final class MaiSystemCalendarEventsInstrumentedTest {
    @Test public void writesEventAndAlarmToIsolatedSystemCalendar() throws Exception {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        assertTrue(BuildConfig.DEBUG && BuildConfig.APPLICATION_ID.endsWith(".isolated"));
        assertEquals(PackageManager.PERMISSION_GRANTED,
            context.checkSelfPermission(Manifest.permission.READ_CALENDAR));
        assertEquals(PackageManager.PERMISSION_GRANTED,
            context.checkSelfPermission(Manifest.permission.WRITE_CALENDAR));
        ContentResolver resolver = context.getContentResolver();
        try (Cursor existing = resolver.query(CalendarContract.Calendars.CONTENT_URI,
            new String[]{CalendarContract.Calendars._ID}, null, null, null)) {
            Assume.assumeTrue("Only run on an empty test calendar provider",
                existing != null && existing.getCount() == 0);
        }

        String account = "maichat-calendar-test-" + UUID.randomUUID();
        Uri syncCalendars = CalendarContract.Calendars.CONTENT_URI.buildUpon()
            .appendQueryParameter(CalendarContract.CALLER_IS_SYNCADAPTER, "true")
            .appendQueryParameter(CalendarContract.Calendars.ACCOUNT_NAME, account)
            .appendQueryParameter(CalendarContract.Calendars.ACCOUNT_TYPE,
                CalendarContract.ACCOUNT_TYPE_LOCAL)
            .build();
        ContentValues calendar = new ContentValues();
        calendar.put(CalendarContract.Calendars.ACCOUNT_NAME, account);
        calendar.put(CalendarContract.Calendars.ACCOUNT_TYPE,
            CalendarContract.ACCOUNT_TYPE_LOCAL);
        calendar.put(CalendarContract.Calendars.NAME, account);
        calendar.put(CalendarContract.Calendars.CALENDAR_DISPLAY_NAME,
            "MaiChat isolated calendar test");
        calendar.put(CalendarContract.Calendars.CALENDAR_COLOR, Color.BLUE);
        calendar.put(CalendarContract.Calendars.CALENDAR_ACCESS_LEVEL,
            CalendarContract.Calendars.CAL_ACCESS_OWNER);
        calendar.put(CalendarContract.Calendars.OWNER_ACCOUNT, account);
        calendar.put(CalendarContract.Calendars.VISIBLE, 1);
        calendar.put(CalendarContract.Calendars.SYNC_EVENTS, 1);
        calendar.put(CalendarContract.Calendars.IS_PRIMARY, 1);
        calendar.put(CalendarContract.Calendars.ALLOWED_REMINDERS,
            Integer.toString(CalendarContract.Reminders.METHOD_ALERT));
        calendar.put(CalendarContract.Calendars.MAX_REMINDERS, 1);
        Uri createdCalendar = resolver.insert(syncCalendars, calendar);
        assertNotNull("Test calendar provider refused a local calendar", createdCalendar);
        long calendarId = ContentUris.parseId(createdCalendar);
        try {
            long start = System.currentTimeMillis() + 5 * 60_000L;
            JSONObject result = MaiSystemCalendarEvents.create(context,
                new JSONObject().put("title", "MaiChat calendar test")
                    .put("notes", "Isolated emulator only")
                    .put("session_id", "ses_calendar_test")
                    .put("starts_at_utc_ms", start)
                    .put("ends_at_utc_ms", start + 30 * 60_000L));
            assertEquals("created", result.getString("state"));
            assertEquals(calendarId, result.getLong("calendar_id"));
            assertTrue("System calendar did not save the start-time alarm",
                result.getBoolean("reminder_created"));
            long eventId = result.getLong("event_id");
            try (Cursor events = resolver.query(ContentUris.withAppendedId(
                    CalendarContract.Events.CONTENT_URI, eventId),
                    new String[]{CalendarContract.Events.TITLE,
                        CalendarContract.Events.DTSTART}, null, null, null)) {
                assertNotNull(events);
                assertTrue(events.moveToFirst());
                assertEquals("MaiChat calendar test", events.getString(0));
                assertEquals(start, events.getLong(1));
            }
            try (Cursor reminders = resolver.query(CalendarContract.Reminders.CONTENT_URI,
                    new String[]{CalendarContract.Reminders.MINUTES,
                        CalendarContract.Reminders.METHOD},
                    CalendarContract.Reminders.EVENT_ID + "=?",
                    new String[]{Long.toString(eventId)}, null)) {
                assertNotNull(reminders);
                assertTrue(reminders.moveToFirst());
                assertEquals(0, reminders.getInt(0));
                assertEquals(CalendarContract.Reminders.METHOD_ALERT,
                    reminders.getInt(1));
            }
        } finally {
            resolver.delete(ContentUris.withAppendedId(syncCalendars, calendarId),
                null, null);
        }
    }
}
