package com.kongshang.maichat.tools;

import android.Manifest;
import android.content.Context;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.os.Build;
import android.provider.Telephony;
import android.telephony.SubscriptionManager;
import android.telephony.TelephonyManager;
import com.kongshang.maichat.MainActivity;
import org.json.JSONArray;
import org.json.JSONObject;

/** Reads only the telephony data explicitly authorized for the foreground app. */
final class MobileTelephonyTools {
    private final MainActivity activity;

    MobileTelephonyTools(MainActivity activity) {
        this.activity = activity;
    }

    JSONObject getPhoneNumber() throws Exception {
        if (activity.checkSelfPermission(Manifest.permission.READ_PHONE_NUMBERS)
                != PackageManager.PERMISSION_GRANTED)
            return denied("phone_number");
        if (!activity.isHostToolForeground())
            return unavailable("Open MaiChat before reading the phone number.");
        if (!activity.getPackageManager().hasSystemFeature(PackageManager.FEATURE_TELEPHONY))
            return unavailable("This device has no cellular service.");
        try {
            String number;
            if (Build.VERSION.SDK_INT >= 33) {
                SubscriptionManager subscriptions = (SubscriptionManager)
                    activity.getSystemService(Context.TELEPHONY_SUBSCRIPTION_SERVICE);
                if (subscriptions == null)
                    return unavailable("The cellular subscription is unavailable.");
                number = subscriptions.getPhoneNumber(
                    SubscriptionManager.DEFAULT_SUBSCRIPTION_ID);
            } else {
                TelephonyManager telephony = (TelephonyManager)
                    activity.getSystemService(Context.TELEPHONY_SERVICE);
                if (telephony == null)
                    return unavailable("The cellular service is unavailable.");
                number = telephony.getLine1Number();
            }
            if (number == null || number.trim().isEmpty())
                return unavailable("The carrier did not provide a phone number for the default subscription.");
            return new JSONObject().put("phone_number", number.trim())
                .put("verified", false).put("source", "default_subscription");
        } catch (SecurityException denied) {
            return denied("phone_number");
        } catch (IllegalStateException unavailable) {
            return unavailable("The cellular service is temporarily unavailable.");
        } catch (UnsupportedOperationException unavailable) {
            return unavailable("This device cannot provide its subscription number.");
        }
    }

    JSONObject readSms(JSONObject arguments) throws Exception {
        int limit = arguments.optInt("limit", 10);
        long afterMs = arguments.optLong("after_ms", 0);
        if (limit < 1 || limit > 20 || afterMs < 0)
            throw new IllegalArgumentException("limit must be 1-20 and after_ms must be nonnegative");
        if (activity.checkSelfPermission(Manifest.permission.READ_SMS)
                != PackageManager.PERMISSION_GRANTED)
            return denied("sms");
        if (!activity.isHostToolForeground())
            return unavailable("Open MaiChat before reading SMS messages.");

        String[] columns = {Telephony.Sms._ID, Telephony.Sms.ADDRESS, Telephony.Sms.BODY,
            Telephony.Sms.DATE, Telephony.Sms.READ};
        String selection = afterMs > 0 ? Telephony.Sms.DATE + " >= ?" : null;
        String[] selectionArgs = afterMs > 0 ? new String[]{Long.toString(afterMs)} : null;
        JSONArray messages = new JSONArray();
        try (Cursor cursor = activity.getContentResolver().query(Telephony.Sms.Inbox.CONTENT_URI,
                 columns, selection, selectionArgs, Telephony.Sms.DATE + " DESC")) {
            if (cursor == null) return unavailable("The SMS inbox is unavailable.");
            int idIndex = cursor.getColumnIndexOrThrow(Telephony.Sms._ID);
            int addressIndex = cursor.getColumnIndexOrThrow(Telephony.Sms.ADDRESS);
            int bodyIndex = cursor.getColumnIndexOrThrow(Telephony.Sms.BODY);
            int dateIndex = cursor.getColumnIndexOrThrow(Telephony.Sms.DATE);
            int readIndex = cursor.getColumnIndexOrThrow(Telephony.Sms.READ);
            while (messages.length() < limit && cursor.moveToNext()) {
                String sender = cursor.getString(addressIndex);
                String body = cursor.getString(bodyIndex);
                messages.put(new JSONObject()
                    .put("id", cursor.getLong(idIndex))
                    .put("sender", sender == null ? "" : sender)
                    .put("body", body == null ? "" : body)
                    .put("timestamp_ms", cursor.getLong(dateIndex))
                    .put("read", cursor.getInt(readIndex) != 0));
            }
        } catch (SecurityException denied) {
            return denied("sms");
        }
        return new JSONObject().put("messages", messages).put("count", messages.length());
    }

    private static JSONObject denied(String permission) throws Exception {
        return new JSONObject().put("code", "permission_denied")
            .put("permission", permission).put("settings_required", false)
            .put("message", "Call mobile_request_permission first.");
    }

    private static JSONObject unavailable(String message) throws Exception {
        return new JSONObject().put("code", "unavailable").put("message", message);
    }
}
