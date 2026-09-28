package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import java.io.File;
import java.io.FileOutputStream;
import org.json.JSONObject;
import org.junit.Test;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
public final class MobileImageVisionInstrumentedTest {
    @Test
    public void detectsFacesAndWritesPersonMaskFromWorkspaceImage() throws Exception {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        Bitmap atlas = BitmapFactory.decodeResource(context.getResources(),
            R.drawable.avatar_chibi_atlas_v3);
        assertNotNull(atlas);
        Bitmap portrait = Bitmap.createBitmap(atlas, 0, 0,
            atlas.getWidth() / 6, atlas.getHeight() / 5);
        File source = new File(context.getCacheDir(), "vision-portrait-test.png");
        File mask = new File(context.getCacheDir(), "vision-person-mask-test.png");
        try {
            try (FileOutputStream output = new FileOutputStream(source)) {
                assertTrue(portrait.compress(Bitmap.CompressFormat.PNG, 100, output));
            }
            JSONObject faces = MobileImageVision.detectFaces(source);
            assertEquals(portrait.getWidth(), faces.getInt("source_width"));
            assertEquals(portrait.getHeight(), faces.getInt("source_height"));
            assertNotNull(faces.getJSONArray("faces"));
            JSONObject result = MobileImageVision.segmentPerson(source, mask);
            assertTrue(mask.isFile() && mask.length() > 0);
            Bitmap decoded = BitmapFactory.decodeFile(mask.getPath());
            assertNotNull(decoded);
            assertEquals(result.getInt("mask_width"), decoded.getWidth());
            assertEquals(result.getInt("mask_height"), decoded.getHeight());
            decoded.recycle();
        } finally {
            source.delete();
            mask.delete();
            portrait.recycle();
            atlas.recycle();
        }
    }
}
