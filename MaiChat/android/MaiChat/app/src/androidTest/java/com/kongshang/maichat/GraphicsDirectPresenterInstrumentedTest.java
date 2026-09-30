package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.Color;
import android.widget.FrameLayout;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import java.io.File;
import java.io.FileOutputStream;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.Test;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
public final class GraphicsDirectPresenterInstrumentedTest {
    @Test public void effectRendersStraightToTextureView() throws Exception {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        File source = new File(context.getCacheDir(), "graphics-direct-red.ppm");
        byte[] ppm = {
            'P', '6', '\n', '2', ' ', '2', '\n', '2', '5', '5', '\n',
            (byte) 255, 0, 0, (byte) 255, 0, 0,
            (byte) 255, 0, 0, (byte) 255, 0, 0
        };
        try (FileOutputStream output = new FileOutputStream(source)) { output.write(ppm); }

        Intent intent = new Intent(context, MainActivity.class);
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        Activity activity = InstrumentationRegistry.getInstrumentation().startActivitySync(intent);
        CountDownLatch presented = new CountDownLatch(1);
        AtomicReference<MaiGraphicsImageView> surface = new AtomicReference<>();
        AtomicReference<Boolean> success = new AtomicReference<>(false);
        try {
            InstrumentationRegistry.getInstrumentation().runOnMainSync(() -> {
                MaiGraphicsImageView view = new MaiGraphicsImageView(activity);
                view.setPresentationListener(rendered -> {
                    success.set(rendered);
                    presented.countDown();
                });
                view.showImage(source.getAbsolutePath(), 64, 64, false, null);
                ((FrameLayout) activity.findViewById(android.R.id.content)).addView(view,
                    new FrameLayout.LayoutParams(64, 64));
                surface.set(view);
            });
            assertTrue(presented.await(10, TimeUnit.SECONDS));
            assertTrue(success.get());
            File effects = new File(context.getFilesDir(), "MaiAgentGraphics");
            assertTrue(new File(effects, "default.effect").isFile());
            assertTrue(new File(effects, "solid.effect").isFile());
            Thread.sleep(250);
            AtomicReference<Bitmap> captured = new AtomicReference<>();
            InstrumentationRegistry.getInstrumentation().runOnMainSync(
                () -> captured.set(((MaiGraphicsTextureView) surface.get().getChildAt(0))
                    .getBitmap()));
            assertNotNull(captured.get());
            assertEquals(Color.RED, captured.get().getPixel(32, 32));
        } finally {
            InstrumentationRegistry.getInstrumentation().runOnMainSync(() -> {
                if (surface.get() != null)
                    ((FrameLayout) activity.findViewById(android.R.id.content))
                        .removeView(surface.get());
                activity.finish();
            });
            source.delete();
        }
    }
}
