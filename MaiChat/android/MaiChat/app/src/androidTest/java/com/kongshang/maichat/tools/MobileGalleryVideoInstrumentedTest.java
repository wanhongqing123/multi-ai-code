package com.kongshang.maichat.tools;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;

import android.Manifest;
import android.app.Activity;
import android.content.ContentValues;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.provider.MediaStore;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import com.kongshang.maichat.AIAssistantController;
import com.kongshang.maichat.MainActivity;
import java.io.File;
import java.io.InputStream;
import java.io.OutputStream;
import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.Test;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
public final class MobileGalleryVideoInstrumentedTest {
    @Test public void listsAndExportsVideoBytesInsteadOfItsCover() throws Exception {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        InstrumentationRegistry.getInstrumentation().getUiAutomation()
            .grantRuntimePermission(context.getPackageName(), Manifest.permission.READ_MEDIA_IMAGES);
        InstrumentationRegistry.getInstrumentation().getUiAutomation()
            .grantRuntimePermission(context.getPackageName(), Manifest.permission.READ_MEDIA_VIDEO);
        Activity activity = InstrumentationRegistry.getInstrumentation().startActivitySync(
            new Intent(context, MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        Uri created = null;
        File exported = null;
        try {
            ContentValues values = new ContentValues();
            values.put(MediaStore.MediaColumns.DISPLAY_NAME, "maichat-gallery-test.mp4");
            values.put(MediaStore.MediaColumns.MIME_TYPE, "video/mp4");
            values.put(MediaStore.Video.Media.DATE_TAKEN, System.currentTimeMillis());
            values.put(MediaStore.MediaColumns.RELATIVE_PATH, "Movies/MaiChatTest/");
            values.put(MediaStore.MediaColumns.IS_PENDING, 1);
            created = activity.getContentResolver().insert(
                MediaStore.Video.Media.getContentUri(MediaStore.VOLUME_EXTERNAL_PRIMARY), values);
            assertNotNull(created);
            try (InputStream input = InstrumentationRegistry.getInstrumentation()
                    .getContext().getAssets().open("gallery-sample.mp4");
                 OutputStream output = activity.getContentResolver().openOutputStream(created)) {
                assertNotNull(output);
                byte[] buffer = new byte[8192];
                int count;
                while ((count = input.read(buffer)) >= 0) output.write(buffer, 0, count);
            }
            ContentValues visible = new ContentValues();
            visible.put(MediaStore.MediaColumns.IS_PENDING, 0);
            activity.getContentResolver().update(created, visible, null, null);

            MobilePhotoTools tools = new MobilePhotoTools((MainActivity) activity);
            MobilePermissionTools permissions = new MobilePermissionTools(
                (MainActivity) activity, tools);
            JSONObject photoPermission = permissions.request(
                new JSONObject().put("permission", "photos"));
            assertEquals("granted", photoPermission.getString("status"));
            InstrumentationRegistry.getInstrumentation().getUiAutomation()
                .grantRuntimePermission(context.getPackageName(),
                    Manifest.permission.ACCESS_COARSE_LOCATION);
            InstrumentationRegistry.getInstrumentation().getUiAutomation()
                .grantRuntimePermission(context.getPackageName(),
                    Manifest.permission.ACCESS_FINE_LOCATION);
            JSONObject locationPermission = permissions.request(
                new JSONObject().put("permission", "location"));
            assertEquals("granted", locationPermission.getString("status"));
            JSONObject list = (JSONObject) tools.execute("mobile_list_photos",
                new JSONObject().put("limit", 100));
            String id = Long.toString(android.content.ContentUris.parseId(created));
            JSONArray items = list.getJSONArray("items");
            JSONObject item = null;
            for (int index = 0; index < items.length(); ++index)
                if (id.equals(items.getJSONObject(index).getString("id")))
                    item = items.getJSONObject(index);
            assertNotNull("video missing from gallery list", item);
            assertEquals("video", item.getString("mediaType"));

            JSONObject original = (JSONObject) tools.execute("mobile_export_media_original",
                new JSONObject().put("id", id));
            assertEquals("video", original.getString("mediaType"));
            assertEquals("video/mp4", original.getString("mime_type"));
            exported = AIAssistantController.shared(activity)
                .workspaceFile(original.getString("path"));
            assertNotNull(exported);
            assertTrue(exported.isFile());
            assertTrue(exported.getName().endsWith(".mp4"));
            assertTrue(exported.length() > 1000);
            try (InputStream input = new java.io.FileInputStream(exported)) {
                byte[] header = new byte[12];
                assertEquals(12, input.read(header));
                assertEquals("ftyp", new String(header, 4, 4,
                    java.nio.charset.StandardCharsets.US_ASCII));
            }
        } finally {
            if (exported != null) exported.delete();
            if (created != null) activity.getContentResolver().delete(created, null, null);
            InstrumentationRegistry.getInstrumentation().runOnMainSync(activity::finish);
        }
    }
}
