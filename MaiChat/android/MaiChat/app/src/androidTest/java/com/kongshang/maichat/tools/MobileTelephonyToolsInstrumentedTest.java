package com.kongshang.maichat.tools;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotEquals;
import static org.junit.Assert.assertTrue;

import android.Manifest;
import android.app.UiAutomation;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import com.kongshang.maichat.MainActivity;
import org.json.JSONObject;
import org.junit.Test;
import org.junit.Assume;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
public final class MobileTelephonyToolsInstrumentedTest {
    @Test public void grantedSmsPermissionReadsABoundedInboxResult() throws Exception {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        Assume.assumeTrue(context.checkSelfPermission(Manifest.permission.READ_SMS)
            == PackageManager.PERMISSION_GRANTED);
        MainActivity activity = (MainActivity) InstrumentationRegistry.getInstrumentation()
            .startActivitySync(new Intent(context, MainActivity.class)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        try {
            JSONObject result = new MobileTelephonyTools(activity).readSms(
                new JSONObject().put("limit", 1));
            assertTrue(result.toString(), result.has("messages"));
            assertTrue(result.getJSONArray("messages").length() <= 1);
            String expected = InstrumentationRegistry.getArguments().getString("expectedSmsBody");
            if (expected != null) {
                assertEquals(1, result.getJSONArray("messages").length());
                assertEquals(expected, result.getJSONArray("messages")
                    .getJSONObject(0).getString("body"));
            }
        } finally {
            InstrumentationRegistry.getInstrumentation().runOnMainSync(activity::finish);
        }
    }

    @Test public void readsOnlyAfterPhonePermissionAndNeverReadsSmsWithoutItsGrant()
            throws Exception {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        UiAutomation automation = InstrumentationRegistry.getInstrumentation().getUiAutomation();
        Assume.assumeTrue(context.checkSelfPermission(Manifest.permission.READ_PHONE_NUMBERS)
            != PackageManager.PERMISSION_GRANTED);
        Assume.assumeTrue(context.checkSelfPermission(Manifest.permission.READ_SMS)
            != PackageManager.PERMISSION_GRANTED);
        MainActivity activity = (MainActivity) InstrumentationRegistry.getInstrumentation()
            .startActivitySync(new Intent(context, MainActivity.class)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        try {
            MobileTelephonyTools tools = new MobileTelephonyTools(activity);
            assertEquals("permission_denied", tools.getPhoneNumber().getString("code"));
            assertEquals("permission_denied", tools.readSms(new JSONObject()).getString("code"));

            automation.grantRuntimePermission(context.getPackageName(),
                Manifest.permission.READ_PHONE_NUMBERS);
            JSONObject permission = new MobilePermissionTools(activity,
                new MobilePhotoTools(activity)).request(
                    new JSONObject().put("permission", "phone_number"));
            assertEquals("granted", permission.getString("status"));
            assertNotEquals("permission_denied", tools.getPhoneNumber().optString("code"));
        } finally {
            InstrumentationRegistry.getInstrumentation().runOnMainSync(activity::finish);
        }
    }
}
