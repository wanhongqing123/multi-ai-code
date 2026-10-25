// 文件作用：Android 移动端 Agent 工具与宿主能力；MobileTelephonyTools 负责 Agent 工具注册、调用与结果返回，本文件给出其实现。
// 主要入口：denied、unavailable。
// 直接依赖：com.kongshang.maichat.MainActivity；实现与这些接口协作。
package com.kongshang.maichat.tools;

import android.Manifest;
import android.content.ActivityNotFoundException;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.provider.Telephony;
import android.telephony.SubscriptionManager;
import android.telephony.TelephonyManager;
import com.kongshang.maichat.MainActivity;
import org.json.JSONArray;
import org.json.JSONObject;

/** Reads authorized telephony data and places user-requested calls. */
final class MobileTelephonyTools {
    interface PhoneCallHost {
        boolean isForeground();
        boolean hasTelephony();
        boolean hasCallPermission();
        boolean requestCallPermission() throws Exception;
        void startCall(Intent intent);
    }

    private final MainActivity activity;
    private final PhoneCallHost phoneCallHost;

    MobileTelephonyTools(MainActivity activity, MobilePermissionTools permissions) {
        this.activity = activity;
        this.phoneCallHost = new PhoneCallHost() {
            @Override public boolean isForeground() {
                return activity.isHostToolForeground();
            }
            @Override public boolean hasTelephony() {
                return activity.getPackageManager().hasSystemFeature(
                    PackageManager.FEATURE_TELEPHONY);
            }
            @Override public boolean hasCallPermission() {
                return activity.checkSelfPermission(Manifest.permission.CALL_PHONE)
                    == PackageManager.PERMISSION_GRANTED;
            }
            @Override public boolean requestCallPermission() throws Exception {
                JSONObject result = permissions.request(
                    new JSONObject().put("permission", "phone_call"));
                return "granted".equals(result.optString("status"));
            }
            @Override public void startCall(Intent intent) {
                activity.startActivity(intent);
            }
        };
    }

    MobileTelephonyTools(PhoneCallHost host) {
        this.activity = null;
        this.phoneCallHost = host;
    }

    JSONObject dial(JSONObject arguments) throws Exception {
        String number = normalizePhoneNumber(arguments.optString("phone_number", ""));
        if (number == null) return new JSONObject().put("code", "invalid_input")
            .put("message", "A valid phone number is required.");
        if (!phoneCallHost.isForeground())
            return unavailable("Open MaiChat before placing a phone call.");
        if (!phoneCallHost.hasTelephony())
            return unavailable("This device cannot place phone calls.");
        // READ_CONTACTS exposes numbers but never grants Android's direct-call permission.
        if (!phoneCallHost.hasCallPermission()) {
            if (!phoneCallHost.requestCallPermission() || !phoneCallHost.hasCallPermission())
                return callPermissionDenied();
        }
        if (!phoneCallHost.isForeground())
            return unavailable("Open MaiChat before placing a phone call.");
        Intent intent = new Intent(Intent.ACTION_CALL, Uri.fromParts("tel", number, null));
        try {
            phoneCallHost.startCall(intent);
        } catch (SecurityException denied) {
            return callPermissionDenied();
        } catch (ActivityNotFoundException unavailable) {
            return unavailable("No phone app can place this call.");
        }
        return new JSONObject().put("status", "call_requested")
            .put("phone_number", number);
    }

    private static String normalizePhoneNumber(String raw) {
        String text = raw == null ? "" : raw.trim();
        if (text.length() > 32) return null;
        StringBuilder number = new StringBuilder();
        int digits = 0;
        for (int i = 0; i < text.length(); i++) {
            char value = text.charAt(i);
            if (value >= '0' && value <= '9') {
                number.append(value);
                digits++;
            } else if (value == '+' && number.length() == 0) {
                number.append(value);
            } else if (value != ' ' && value != '-' && value != '(' && value != ')') {
                return null;
            }
        }
        return digits >= 3 && digits <= 15 ? number.toString() : null;
    }

    private static JSONObject callPermissionDenied() throws Exception {
        return new JSONObject().put("code", "permission_denied")
            .put("permission", "phone_call").put("settings_required", false)
            .put("message", "CALL_PHONE was not granted. Do not retry without a new user request.");
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
