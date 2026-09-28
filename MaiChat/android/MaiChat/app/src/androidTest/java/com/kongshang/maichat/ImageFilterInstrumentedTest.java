package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;

import android.graphics.Bitmap;
import android.graphics.Color;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import java.lang.reflect.Method;
import java.nio.ByteBuffer;
import org.junit.Test;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
public final class ImageFilterInstrumentedTest {
    @Test
    public void nativeFilterKeepsRgbaColorsAndCreatesNewPixels() throws Exception {
        System.loadLibrary("maichat_agent");
        Bitmap source = Bitmap.createBitmap(2, 1, Bitmap.Config.ARGB_8888);
        source.setPixel(0, 0, Color.RED);
        source.setPixel(1, 0, Color.GREEN);
        Method filter = AIAssistantController.class.getDeclaredMethod(
            "nativeFilterBitmap", Bitmap.class, String.class, int[].class);
        filter.setAccessible(true);
        int[] dimensions = new int[2];
        byte[] result = (byte[]) filter.invoke(null, source,
            "{\"operation\":\"flip_horizontal\"}", dimensions);
        assertNotNull(result);
        assertEquals(2, dimensions[0]);
        assertEquals(1, dimensions[1]);
        Bitmap output = Bitmap.createBitmap(dimensions[0], dimensions[1], Bitmap.Config.ARGB_8888);
        output.copyPixelsFromBuffer(ByteBuffer.wrap(result));
        assertEquals(Color.GREEN, output.getPixel(0, 0));
        assertEquals(Color.RED, output.getPixel(1, 0));
        output.recycle();
        source.recycle();
    }
}
