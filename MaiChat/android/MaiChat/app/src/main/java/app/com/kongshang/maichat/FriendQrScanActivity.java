// 文件作用：扫描 MaiChat 好友二维码，只返回经过格式校验的 MaiChat 账号 ID。
package com.kongshang.maichat;

import android.Manifest;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.media.Image;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.widget.FrameLayout;
import android.widget.TextView;

import androidx.activity.ComponentActivity;
import androidx.camera.core.CameraSelector;
import androidx.camera.core.ImageAnalysis;
import androidx.camera.core.ImageProxy;
import androidx.camera.core.Preview;
import androidx.camera.lifecycle.ProcessCameraProvider;
import androidx.camera.view.PreviewView;
import androidx.core.content.ContextCompat;

import com.google.common.util.concurrent.ListenableFuture;
import com.google.mlkit.vision.barcode.BarcodeScanner;
import com.google.mlkit.vision.barcode.BarcodeScannerOptions;
import com.google.mlkit.vision.barcode.BarcodeScanning;
import com.google.mlkit.vision.barcode.common.Barcode;
import com.google.mlkit.vision.common.InputImage;

import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicBoolean;

public final class FriendQrScanActivity extends ComponentActivity {
    private static final int CAMERA_PERMISSION_REQUEST = 1;
    private final ExecutorService cameraExecutor = Executors.newSingleThreadExecutor();
    private final AtomicBoolean frameInFlight = new AtomicBoolean();
    private final BarcodeScanner scanner = BarcodeScanning.getClient(
        new BarcodeScannerOptions.Builder().setBarcodeFormats(Barcode.FORMAT_QR_CODE).build());
    private PreviewView previewView;
    private TextView hint;
    private ProcessCameraProvider provider;
    private boolean providerLoading;
    private volatile boolean finished;

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(0xFF101829);
        previewView = new PreviewView(this);
        root.addView(previewView, new FrameLayout.LayoutParams(-1, -1));
        View frame = new View(this);
        frame.setBackground(MaiChatTheme.bordered(Color.TRANSPARENT, Color.WHITE, 18, this));
        FrameLayout.LayoutParams frameParams = new FrameLayout.LayoutParams(dp(250), dp(250),
            Gravity.CENTER);
        root.addView(frame, frameParams);
        TextView title = MaiChatTheme.label(this, "扫一扫", 21, Color.WHITE);
        title.setGravity(Gravity.CENTER);
        FrameLayout.LayoutParams titleParams = new FrameLayout.LayoutParams(dp(150), dp(50),
            Gravity.TOP | Gravity.CENTER_HORIZONTAL);
        titleParams.topMargin = dp(12);
        root.addView(title, titleParams);
        hint = MaiChatTheme.label(this, "扫描对方的好友二维码", 16, Color.WHITE);
        hint.setGravity(Gravity.CENTER);
        FrameLayout.LayoutParams hintParams = new FrameLayout.LayoutParams(-1, dp(58),
            Gravity.BOTTOM);
        hintParams.bottomMargin = dp(55);
        root.addView(hint, hintParams);
        TextView close = MaiChatTheme.label(this, "关闭", 16, Color.WHITE);
        close.setGravity(Gravity.CENTER);
        close.setOnClickListener(view -> finish());
        FrameLayout.LayoutParams closeParams = new FrameLayout.LayoutParams(dp(70), dp(50),
            Gravity.TOP | Gravity.START);
        closeParams.leftMargin = dp(12);
        closeParams.topMargin = dp(12);
        root.addView(close, closeParams);
        setContentView(root);
    }

    @Override protected void onResume() {
        super.onResume();
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.CAMERA)
            == PackageManager.PERMISSION_GRANTED) {
            ensureCamera();
        } else {
            hint.setText("请允许 MaiChat 使用相机扫码");
            requestPermissions(new String[]{Manifest.permission.CAMERA}, CAMERA_PERMISSION_REQUEST);
        }
    }

    @Override protected void onPause() {
        if (provider != null) provider.unbindAll();
        super.onPause();
    }

    @Override public void onRequestPermissionsResult(int requestCode, String[] permissions,
                                                      int[] results) {
        super.onRequestPermissionsResult(requestCode, permissions, results);
        if (requestCode != CAMERA_PERMISSION_REQUEST) return;
        if (results.length > 0 && results[0] == PackageManager.PERMISSION_GRANTED) {
            ensureCamera();
        } else {
            hint.setText("相机权限未开启，请到系统设置允许 MaiChat 使用相机");
        }
    }

    private void ensureCamera() {
        if (finished || providerLoading) return;
        if (provider != null) { bindCamera(); return; }
        providerLoading = true;
        ListenableFuture<ProcessCameraProvider> future = ProcessCameraProvider.getInstance(this);
        future.addListener(() -> {
            providerLoading = false;
            if (isFinishing() || isDestroyed()) return;
            try {
                provider = future.get();
                bindCamera();
            } catch (Exception failure) { hint.setText("相机不可用"); }
        }, ContextCompat.getMainExecutor(this));
    }

    private void bindCamera() {
        if (finished || provider == null || isFinishing()) return;
        try {
            Preview preview = new Preview.Builder().build();
            preview.setSurfaceProvider(previewView.getSurfaceProvider());
            ImageAnalysis analysis = new ImageAnalysis.Builder()
                .setBackpressureStrategy(ImageAnalysis.STRATEGY_KEEP_ONLY_LATEST).build();
            analysis.setAnalyzer(cameraExecutor, this::scanFrame);
            provider.unbindAll();
            provider.bindToLifecycle(this, CameraSelector.DEFAULT_BACK_CAMERA, preview, analysis);
            hint.setText("扫描对方的好友二维码");
        } catch (Exception failure) { hint.setText("相机不可用"); }
    }

    @androidx.camera.core.ExperimentalGetImage
    private void scanFrame(ImageProxy frame) {
        if (finished || !frameInFlight.compareAndSet(false, true)) {
            frame.close();
            return;
        }
        Image image = frame.getImage();
        if (image == null) { frameInFlight.set(false); frame.close(); return; }
        InputImage input = InputImage.fromMediaImage(image,
            frame.getImageInfo().getRotationDegrees());
        scanner.process(input).addOnSuccessListener(codes -> {
            for (Barcode code : codes) {
                String userId = FriendQrPolicy.userId(code.getRawValue());
                if (userId.isEmpty()) {
                    hint.setText("这不是 MaiChat 好友二维码");
                    continue;
                }
                if (finished) return;
                finished = true;
                Intent result = new Intent().putExtra("friend_user_id", userId);
                setResult(RESULT_OK, result);
                finish();
                return;
            }
        }).addOnCompleteListener(task -> {
            frameInFlight.set(false);
            frame.close();
        });
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    @Override protected void onDestroy() {
        finished = true;
        if (provider != null) provider.unbindAll();
        scanner.close();
        cameraExecutor.shutdownNow();
        super.onDestroy();
    }
}
