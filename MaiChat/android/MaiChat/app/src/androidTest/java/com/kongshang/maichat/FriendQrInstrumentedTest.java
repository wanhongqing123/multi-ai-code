// 文件作用：在 Android 设备上验证自己的好友二维码可被同一扫码栈解码。
package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

import android.graphics.Bitmap;
import android.view.View;
import android.view.ViewGroup;
import android.widget.TextView;

import androidx.test.core.app.ActivityScenario;
import androidx.test.ext.junit.runners.AndroidJUnit4;

import com.google.android.gms.tasks.Tasks;
import com.google.mlkit.vision.barcode.BarcodeScanner;
import com.google.mlkit.vision.barcode.BarcodeScannerOptions;
import com.google.mlkit.vision.barcode.BarcodeScanning;
import com.google.mlkit.vision.barcode.common.Barcode;
import com.google.mlkit.vision.common.InputImage;
import com.google.zxing.BarcodeFormat;
import com.google.zxing.common.BitMatrix;
import com.google.zxing.qrcode.QRCodeWriter;

import org.junit.Test;
import org.junit.runner.RunWith;

import java.util.List;
import java.util.concurrent.TimeUnit;

@RunWith(AndroidJUnit4.class)
public class FriendQrInstrumentedTest {
    @Test public void scannerOpensWithFriendPrompt() throws Exception {
        try (ActivityScenario<FriendQrScanActivity> scenario =
                 ActivityScenario.launch(FriendQrScanActivity.class)) {
            scenario.onActivity(activity -> assertTrue(
                hasText(activity.getWindow().getDecorView(), "扫描对方的好友二维码")));
            Thread.sleep(800);
        }
    }

    private boolean hasText(View view, String text) {
        if (view instanceof TextView && text.contentEquals(((TextView) view).getText())) return true;
        if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) view;
            for (int index = 0; index < group.getChildCount(); index++)
                if (hasText(group.getChildAt(index), text)) return true;
        }
        return false;
    }

    @Test public void generatedAccountCodeScansBackToSameUser() throws Exception {
        String userId = "mtest_friend_12345";
        BitMatrix matrix = new QRCodeWriter().encode(
            FriendQrPolicy.link(userId), BarcodeFormat.QR_CODE, 600, 600);
        Bitmap bitmap = Bitmap.createBitmap(600, 600, Bitmap.Config.ARGB_8888);
        for (int y = 0; y < 600; y++)
            for (int x = 0; x < 600; x++)
                bitmap.setPixel(x, y, matrix.get(x, y) ? 0xFF000000 : 0xFFFFFFFF);
        BarcodeScanner scanner = BarcodeScanning.getClient(
            new BarcodeScannerOptions.Builder().setBarcodeFormats(Barcode.FORMAT_QR_CODE).build());
        try {
            List<Barcode> codes = Tasks.await(scanner.process(InputImage.fromBitmap(bitmap, 0)),
                20, TimeUnit.SECONDS);
            assertTrue("Own QR must be recognized", !codes.isEmpty());
            assertEquals(userId, FriendQrPolicy.userId(codes.get(0).getRawValue()));
        } finally { scanner.close(); }
    }
}
