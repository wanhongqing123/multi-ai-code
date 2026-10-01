package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.Color;
import android.view.View;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.TextView;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.Test;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
public final class GraphicsDirectPresenterInstrumentedTest {
    @Test public void messagePreviewDecodesPngIntoNativeImageView() throws Exception {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        File source = new File(context.getCacheDir(), "message-preview-red.png");
        Bitmap original = Bitmap.createBitmap(24, 16, Bitmap.Config.ARGB_8888);
        original.eraseColor(Color.RED);
        try (FileOutputStream output = new FileOutputStream(source)) {
            assertTrue(original.compress(Bitmap.CompressFormat.PNG, 100, output));
        }
        original.recycle();
        ImageView image = new ImageView(context);
        CountDownLatch decoded = new CountDownLatch(1);
        InstrumentationRegistry.getInstrumentation().runOnMainSync(() -> {
            MessageImageLoader.load(source.getAbsolutePath(), 48, 48, image, decoded::countDown);
        });
        AtomicReference<Bitmap> result = new AtomicReference<>();
        long deadline = System.currentTimeMillis() + 10000;
        while (result.get() == null && System.currentTimeMillis() < deadline) {
            InstrumentationRegistry.getInstrumentation().runOnMainSync(() -> {
                if (image.getDrawable() instanceof android.graphics.drawable.BitmapDrawable)
                    result.set(((android.graphics.drawable.BitmapDrawable) image.getDrawable())
                        .getBitmap());
            });
            Thread.sleep(25);
        }
        assertNotNull("FFmpeg failed to decode the PNG preview", result.get());
        assertEquals(Color.RED, result.get().getPixel(8, 8));
        assertEquals(1L, decoded.getCount());
    }

    @Test public void effectRendersStraightToTextureView() throws Exception {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        File source = new File(context.getCacheDir(), "graphics-direct-red.ppm");
        File pngSource = new File(context.getCacheDir(), "graphics-direct-red.png");
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
                view.showImage(source.getAbsolutePath(), false, null);
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

            Bitmap png = Bitmap.createBitmap(2, 2, Bitmap.Config.ARGB_8888);
            png.eraseColor(Color.RED);
            try (FileOutputStream output = new FileOutputStream(pngSource)) {
                assertTrue(png.compress(Bitmap.CompressFormat.PNG, 100, output));
            }
            png.recycle();
            CountDownLatch pngPresented = new CountDownLatch(1);
            AtomicReference<Boolean> pngSuccess = new AtomicReference<>(false);
            InstrumentationRegistry.getInstrumentation().runOnMainSync(() -> {
                surface.get().setPresentationListener(rendered -> {
                    pngSuccess.set(rendered);
                    pngPresented.countDown();
                });
                surface.get().showImage(pngSource.getAbsolutePath(), false, null);
            });
            assertTrue(pngPresented.await(10, TimeUnit.SECONDS));
            assertTrue("PNG must decode through FFmpeg Graphics", pngSuccess.get());

            CountDownLatch nextFrame = new CountDownLatch(1);
            AtomicReference<Boolean> frameSuccess = new AtomicReference<>(false);
            InstrumentationRegistry.getInstrumentation().runOnMainSync(() ->
                surface.get().setPresentationListener(rendered -> {
                    frameSuccess.set(rendered);
                    nextFrame.countDown();
                }));
            byte[] green = {
                0, (byte) 255, 0, (byte) 255, 0, (byte) 255, 0, (byte) 255,
                0, (byte) 255, 0, (byte) 255, 0, (byte) 255, 0, (byte) 255
            };
            assertTrue(((MaiGraphicsTextureView) surface.get().getChildAt(0))
                .showRgbaFrame(green, 2, 2, 8, false));
            assertTrue(nextFrame.await(10, TimeUnit.SECONDS));
            assertTrue(frameSuccess.get());
            Thread.sleep(250);
            InstrumentationRegistry.getInstrumentation().runOnMainSync(
                () -> captured.set(((MaiGraphicsTextureView) surface.get().getChildAt(0))
                    .getBitmap()));
            assertEquals(Color.GREEN, captured.get().getPixel(32, 32));

            File videoFile = new File(context.getCacheDir(), "graphics-direct-red.mp4");
            try (InputStream input = InstrumentationRegistry.getInstrumentation()
                    .getContext().getAssets().open("tiny-red.mp4");
                 FileOutputStream output = new FileOutputStream(videoFile)) {
                byte[] buffer = new byte[8192];
                int count;
                while ((count = input.read(buffer)) != -1) output.write(buffer, 0, count);
            }
            CountDownLatch videoFrames = new CountDownLatch(2);
            InstrumentationRegistry.getInstrumentation().runOnMainSync(() ->
                surface.get().setPresentationListener(rendered -> {
                    if (rendered) videoFrames.countDown();
                }));
            assertTrue(((MaiGraphicsTextureView) surface.get().getChildAt(0))
                .playVideo(videoFile.getAbsolutePath()));
            assertTrue(videoFrames.await(10, TimeUnit.SECONDS));
            ((MaiGraphicsTextureView) surface.get().getChildAt(0)).pauseVideo();
            Thread.sleep(250);
            InstrumentationRegistry.getInstrumentation().runOnMainSync(
                () -> captured.set(((MaiGraphicsTextureView) surface.get().getChildAt(0))
                    .getBitmap()));
            int videoPixel = captured.get().getPixel(32, 32);
            assertTrue("video pixel: " + Integer.toHexString(videoPixel), Color.red(videoPixel) >= 240);
            assertTrue(Color.green(videoPixel) <= 10);
            assertTrue(Color.blue(videoPixel) <= 10);

            ((MaiGraphicsTextureView) surface.get().getChildAt(0)).stopVideo();
            File invalid = new File(context.getCacheDir(), "graphics-invalid.png");
            try (FileOutputStream output = new FileOutputStream(invalid)) {
                output.write("not an image".getBytes("UTF-8"));
            }
            CountDownLatch rejected = new CountDownLatch(1);
            InstrumentationRegistry.getInstrumentation().runOnMainSync(() -> {
                surface.get().setPresentationListener(rendered -> {
                    if (!rendered) rejected.countDown();
                });
                surface.get().showImage(invalid.getAbsolutePath(), false, null);
            });
            assertTrue(rejected.await(10, TimeUnit.SECONDS));
            InstrumentationRegistry.getInstrumentation().runOnMainSync(() -> {
                assertEquals(2, surface.get().getChildCount());
                assertTrue(surface.get().getChildAt(1) instanceof TextView);
                assertTrue(!(surface.get().getChildAt(1) instanceof ImageView));
                assertEquals(View.VISIBLE, surface.get().getChildAt(1).getVisibility());
            });
            invalid.delete();
            videoFile.delete();
        } finally {
            InstrumentationRegistry.getInstrumentation().runOnMainSync(() -> {
                if (surface.get() != null)
                    ((FrameLayout) activity.findViewById(android.R.id.content))
                        .removeView(surface.get());
                activity.finish();
            });
            source.delete();
            pngSource.delete();
        }
    }
}
