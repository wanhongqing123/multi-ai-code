package com.kongshang.maichat.tools;

import android.Manifest;
import android.content.ContentResolver;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.provider.ContactsContract;
import com.kongshang.maichat.MainActivity;
import java.util.HashSet;
import java.util.Locale;
import java.util.Set;
import org.json.JSONArray;
import org.json.JSONObject;

/** Reads only the system contacts made available by the operating system. */
final class MobileContactsTools {
    private final MainActivity activity;

    MobileContactsTools(MainActivity activity) { this.activity = activity; }

    JSONObject list(JSONObject arguments) throws Exception {
        JSONObject unavailable = permissionOrForegroundError();
        if (unavailable != null) return unavailable;
        String query = arguments.optString("query", "").trim();
        int offset = arguments.optInt("offset", 0);
        int limit = arguments.optInt("limit", 20);
        if (query.getBytes(java.nio.charset.StandardCharsets.UTF_8).length > 256 ||
            offset < 0 || offset > 1000 || limit < 1 || limit > 50)
            throw new IllegalArgumentException("query, offset, or limit is invalid");

        JSONArray contacts = new JSONArray();
        int skipped = 0;
        boolean hasMore = false;
        String[] columns = {ContactsContract.Contacts._ID,
            ContactsContract.Contacts.DISPLAY_NAME_PRIMARY};
        try {
            // 只在权限已授予且 App 位于前台时读取系统通讯录；搜索也受同一权限约束。
            Set<Long> dataMatches = query.isEmpty() ? new HashSet<>() : findDataMatches(query);
            try (Cursor cursor = activity.getContentResolver().query(
                 ContactsContract.Contacts.CONTENT_URI, columns, null, null,
                 ContactsContract.Contacts.DISPLAY_NAME_PRIMARY + " COLLATE LOCALIZED ASC")) {
                if (cursor == null) return notAvailable();
                int idIndex = cursor.getColumnIndexOrThrow(ContactsContract.Contacts._ID);
                int nameIndex = cursor.getColumnIndexOrThrow(
                    ContactsContract.Contacts.DISPLAY_NAME_PRIMARY);
                String lower = query.toLowerCase(Locale.ROOT);
                while (cursor.moveToNext()) {
                    long id = cursor.getLong(idIndex);
                    String name = cursor.getString(nameIndex);
                    if (!query.isEmpty() && !dataMatches.contains(id) &&
                        (name == null || !name.toLowerCase(Locale.ROOT).contains(lower))) continue;
                    if (skipped < offset) { skipped++; continue; }
                    if (contacts.length() == limit) { hasMore = true; break; }
                    contacts.put(readContactDetails(id, name));
                }
            }
        } catch (SecurityException denied) {
            return permissionDenied();
        }
        return new JSONObject().put("contacts", contacts).put("offset", offset)
            .put("count", contacts.length()).put("has_more", hasMore).put("access", "full");
    }

    JSONObject get(JSONObject arguments) throws Exception {
        JSONObject unavailable = permissionOrForegroundError();
        if (unavailable != null) return unavailable;
        String text = arguments.optString("id", "").trim();
        if (text.isEmpty() || text.length() > 32)
            throw new IllegalArgumentException("a contact id is required");
        final long id;
        try { id = Long.parseLong(text); }
        catch (NumberFormatException invalid) {
            throw new IllegalArgumentException("contact id must be numeric");
        }
        if (id < 0) throw new IllegalArgumentException("contact id must be nonnegative");
        String[] columns = {ContactsContract.Contacts.DISPLAY_NAME_PRIMARY};
        try (Cursor cursor = activity.getContentResolver().query(
                 ContactsContract.Contacts.CONTENT_URI, columns,
                 ContactsContract.Contacts._ID + "=?", new String[]{text}, null)) {
            if (cursor == null) return notAvailable();
            if (!cursor.moveToFirst()) return new JSONObject().put("code", "not_found")
                .put("message", "Contact is unavailable or not shared");
            return readContactDetails(id, cursor.getString(0));
        } catch (SecurityException denied) {
            return permissionDenied();
        }
    }

    private Set<Long> findDataMatches(String query) {
        Set<Long> matching = new HashSet<>();
        String lower = query.toLowerCase(Locale.ROOT);
        String digits = query.replaceAll("[^0-9]", "");
        String[] columns = {ContactsContract.Data.CONTACT_ID, ContactsContract.Data.DATA1};
        String selection = ContactsContract.Data.MIMETYPE + " IN (?,?)";
        String[] values = {ContactsContract.CommonDataKinds.Phone.CONTENT_ITEM_TYPE,
            ContactsContract.CommonDataKinds.Email.CONTENT_ITEM_TYPE};
        try (Cursor cursor = activity.getContentResolver().query(
                 ContactsContract.Data.CONTENT_URI, columns, selection, values, null)) {
            if (cursor == null) return matching;
            while (cursor.moveToNext()) {
                String value = cursor.getString(1);
                if (value == null) continue;
                if (value.toLowerCase(Locale.ROOT).contains(lower) ||
                    (digits.length() >= 3 &&
                     value.replaceAll("[^0-9]", "").contains(digits)))
                    matching.add(cursor.getLong(0));
            }
        }
        return matching;
    }

    private JSONObject readContactDetails(long id, String name) throws Exception {
        ContentResolver resolver = activity.getContentResolver();
        JSONArray phones = new JSONArray();
        JSONArray emails = new JSONArray();
        try (Cursor cursor = resolver.query(ContactsContract.CommonDataKinds.Phone.CONTENT_URI,
                 new String[]{ContactsContract.CommonDataKinds.Phone.NUMBER},
                 ContactsContract.CommonDataKinds.Phone.CONTACT_ID + "=?",
                 new String[]{Long.toString(id)}, null)) {
            if (cursor != null)
                while (cursor.moveToNext() && phones.length() < 20)
                    phones.put(bounded(cursor.getString(0), 128));
        }
        try (Cursor cursor = resolver.query(ContactsContract.CommonDataKinds.Email.CONTENT_URI,
                 new String[]{ContactsContract.CommonDataKinds.Email.ADDRESS},
                 ContactsContract.CommonDataKinds.Email.CONTACT_ID + "=?",
                 new String[]{Long.toString(id)}, null)) {
            if (cursor != null)
                while (cursor.moveToNext() && emails.length() < 20)
                    emails.put(bounded(cursor.getString(0), 320));
        }
        return new JSONObject().put("id", Long.toString(id)).put("name", bounded(name, 256))
            .put("phone_numbers", phones).put("emails", emails);
    }

    private JSONObject permissionOrForegroundError() throws Exception {
        if (activity.checkSelfPermission(Manifest.permission.READ_CONTACTS)
                != PackageManager.PERMISSION_GRANTED)
            return permissionDenied();
        if (!activity.isHostToolForeground()) return new JSONObject()
            .put("code", "unavailable").put("message", "Open MaiChat to read system contacts");
        return null;
    }

    private static JSONObject permissionDenied() throws Exception {
        return new JSONObject().put("code", "permission_denied")
            .put("permission", "contacts").put("settings_required", false)
            .put("message", "Call mobile_request_permission with contacts first.");
    }

    private static JSONObject notAvailable() throws Exception {
        return new JSONObject().put("code", "unavailable")
            .put("message", "The system contact store is unavailable");
    }

    private static String bounded(String value, int maximum) {
        return value == null ? "" : value.substring(0, Math.min(value.length(), maximum));
    }
}
