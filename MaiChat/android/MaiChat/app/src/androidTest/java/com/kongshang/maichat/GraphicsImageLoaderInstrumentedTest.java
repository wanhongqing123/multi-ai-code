package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Color;
import android.graphics.drawable.BitmapDrawable;
import android.widget.ImageView;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import java.io.File;
import java.io.FileOutputStream;
import java.lang.reflect.Method;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.Test;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
public final class GraphicsImageLoaderInstrumentedTest {
    @Test
    public void ffmpegAndGlesRenderMessageImage() throws Exception {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        File source = new File(context.getCacheDir(), "graphics-red.ppm");
        byte[] ppm = {
            'P', '6', '\n', '2', ' ', '2', '\n', '2', '5', '5', '\n',
            (byte) 255, 0, 0, (byte) 255, 0, 0,
            (byte) 255, 0, 0, (byte) 255, 0, 0
        };
        try {
            try (FileOutputStream output = new FileOutputStream(source)) {
                output.write(ppm);
            }
            System.loadLibrary("maichat_agent");
            String backend = context.getApplicationInfo().nativeLibraryDir
                + "/libmaiagent_obs_gles.so";
            Method render = MessageImageLoader.class.getDeclaredMethod("nativeRenderImage",
                String.class, int.class, int.class, String.class);
            render.setAccessible(true);
            Bitmap nativeBitmap = (Bitmap) render.invoke(null, source.getPath(), 1, 1, backend);
            assertNotNull(nativeBitmap);
            assertEquals(Color.RED, nativeBitmap.getPixel(0, 0));

            ImageView view = new ImageView(context);
            InstrumentationRegistry.getInstrumentation().runOnMainSync(
                () -> MessageImageLoader.load(source.getPath(), 1, 1, view, null));
            AtomicReference<Bitmap> shown = new AtomicReference<>();
            for (int attempt = 0; attempt < 100 && shown.get() == null; ++attempt) {
                InstrumentationRegistry.getInstrumentation().runOnMainSync(() -> {
                    if (view.getDrawable() instanceof BitmapDrawable)
                        shown.set(((BitmapDrawable) view.getDrawable()).getBitmap());
                });
                if (shown.get() == null) Thread.sleep(50);
            }
            assertNotNull(shown.get());
            assertEquals(Color.RED, shown.get().getPixel(0, 0));
        } finally {
            source.delete();
        }
    }
}
