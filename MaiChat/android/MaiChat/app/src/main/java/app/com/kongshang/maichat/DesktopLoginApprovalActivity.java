// 文件作用：已登录的 Android 手机/平板扫描桌面登录二维码并由用户核对后批准。
package com.kongshang.maichat;

import android.Manifest;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.res.Configuration;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.media.Image;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.provider.Settings;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

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

/** 扫描时只接受本产品 URI；展示服务端核对码后才允许批准。 */
public final class DesktopLoginApprovalActivity extends ComponentActivity {
    private enum State { SCANNING, PERMISSION, LOADING, CONFIRM, DECIDING, RESULT, ERROR }

    private static final int CAMERA_PERMISSION_REQUEST = 1;
    private static final String KEY_REQUEST_ID = "request_id";
    private static final String KEY_DISPLAY_CODE = "display_code";
    private static final String KEY_PLATFORM = "platform";
    private static final String KEY_EXPIRES_AT = "expires_at";
    private static final String KEY_STATE = "state";
    private static final String KEY_ERROR = "error";
    private static final String KEY_APPROVED = "approved";
    private static final String KEY_PERMISSION_REQUESTED = "permission_requested";

    private final ExecutorService io = Executors.newSingleThreadExecutor();
    private final ExecutorService cameraExecutor = Executors.newSingleThreadExecutor();
    private final Handler main = new Handler(Looper.getMainLooper());
    private final AtomicBoolean frameInFlight = new AtomicBoolean();
    private final BarcodeScanner barcodeScanner = BarcodeScanning.getClient(
        new BarcodeScannerOptions.Builder().setBarcodeFormats(Barcode.FORMAT_QR_CODE).build());
    private MaiChatAuthClient authClient;
    private ProcessCameraProvider cameraProvider;
    private boolean cameraProviderLoading;
    private PreviewView previewView;
    private TextView scannerHint;
    private TextView expiryLabel;
    private volatile State state = State.SCANNING;
    private String requestId = "";
    private String displayCode = "";
    private String platform = "";
    private String errorMessage = "";
    private long expiresAt;
    private boolean permissionRequested;
    private boolean approved;
    private volatile boolean destroyed;
    private volatile boolean active;
    private final Runnable tick = new Runnable() {
        @Override public void run() {
            if (destroyed || (state != State.CONFIRM && state != State.DECIDING)) return;
            long remaining = Math.max(0, (expiresAt - SystemClock.elapsedRealtime() + 999) / 1000);
            if (remaining <= 0) {
                if (state == State.DECIDING) {
                    if (expiryLabel != null) expiryLabel.setText("正在提交决定…");
                    return;
                }
                errorMessage = "二维码已过期，请在电脑上刷新后重新扫码";
                state = State.ERROR;
                render();
                return;
            }
            if (expiryLabel != null)
                expiryLabel.setText(String.format(java.util.Locale.ROOT,
                    "剩余 %02d:%02d", remaining / 60, remaining % 60));
            main.postDelayed(this, 1000);
        }
    };

    @Override protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().setStatusBarColor(Color.WHITE);
        getWindow().setNavigationBarColor(Color.WHITE);
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR);
        authClient = MaiChatAuthClient.shared(this);
        if (savedInstanceState != null) {
            requestId = savedInstanceState.getString(KEY_REQUEST_ID, "");
            displayCode = savedInstanceState.getString(KEY_DISPLAY_CODE, "");
            platform = savedInstanceState.getString(KEY_PLATFORM, "");
            expiresAt = savedInstanceState.getLong(KEY_EXPIRES_AT);
            errorMessage = savedInstanceState.getString(KEY_ERROR, "");
            approved = savedInstanceState.getBoolean(KEY_APPROVED);
            permissionRequested = savedInstanceState.getBoolean(KEY_PERMISSION_REQUESTED);
            String restored = savedInstanceState.getString(KEY_STATE, State.SCANNING.name());
            try { state = State.valueOf(restored); }
            catch (IllegalArgumentException ignored) { state = State.SCANNING; }
            if (state == State.CONFIRM || state == State.DECIDING || state == State.LOADING) {
                if (requestId.isEmpty() || expiresAt <= SystemClock.elapsedRealtime()
                    && state != State.LOADING) {
                    state = State.ERROR;
                    errorMessage = "二维码已过期，请在电脑上刷新后重新扫码";
                } else {
                    state = State.LOADING;
                }
            }
        }
        render();
        if (state == State.LOADING && !requestId.isEmpty()) fetchRequest(requestId);
    }

    @Override protected void onResume() {
        super.onResume();
        active = true;
        if (state == State.PERMISSION && hasCameraPermission()) {
            state = State.SCANNING;
            render();
        }
        if (state == State.SCANNING) ensureCamera();
    }

    @Override protected void onPause() {
        active = false;
        stopCamera();
        super.onPause();
    }

    @Override public void onConfigurationChanged(Configuration configuration) {
        super.onConfigurationChanged(configuration);
        stopCamera();
        render();
        if (state == State.SCANNING) ensureCamera();
    }

    @Override protected void onSaveInstanceState(Bundle out) {
        out.putString(KEY_REQUEST_ID, requestId);
        out.putString(KEY_DISPLAY_CODE, displayCode);
        out.putString(KEY_PLATFORM, platform);
        out.putLong(KEY_EXPIRES_AT, expiresAt);
        out.putString(KEY_STATE, state.name());
        out.putString(KEY_ERROR, errorMessage);
        out.putBoolean(KEY_APPROVED, approved);
        out.putBoolean(KEY_PERMISSION_REQUESTED, permissionRequested);
        super.onSaveInstanceState(out);
    }

    @Override protected void onDestroy() {
        destroyed = true;
        main.removeCallbacks(tick);
        stopCamera();
        barcodeScanner.close();
        cameraExecutor.shutdown();
        io.shutdown();
        super.onDestroy();
    }

    @Override public void onBackPressed() {
        if (state == State.DECIDING) {
            Toast.makeText(this, "正在提交决定，请稍候", Toast.LENGTH_SHORT).show();
            return;
        }
        if (state == State.CONFIRM) {
            String id = requestId;
            String code = displayCode;
            io.execute(() -> {
                try { authClient.decideDesktopRequest(id, code, false); }
                catch (Exception ignored) { /* 未批准的请求会按服务端时限过期。 */ }
            });
        }
        finish();
    }

    @Override public void onRequestPermissionsResult(
        int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode != CAMERA_PERMISSION_REQUEST) return;
        if (grantResults.length > 0 && grantResults[0] == PackageManager.PERMISSION_GRANTED) {
            state = State.SCANNING;
            render();
            ensureCamera();
        } else {
            state = State.PERMISSION;
            render();
        }
    }

    private boolean hasCameraPermission() {
        return ContextCompat.checkSelfPermission(this, Manifest.permission.CAMERA)
            == PackageManager.PERMISSION_GRANTED;
    }

    private void ensureCamera() {
        if (state != State.SCANNING || destroyed) return;
        if (!hasCameraPermission()) {
            state = State.PERMISSION;
            render();
            if (!permissionRequested) {
                permissionRequested = true;
                requestPermissions(new String[]{Manifest.permission.CAMERA}, CAMERA_PERMISSION_REQUEST);
            }
            return;
        }
        if (cameraProvider != null) {
            bindCamera();
            return;
        }
        if (cameraProviderLoading) return;
        cameraProviderLoading = true;
        ListenableFuture<ProcessCameraProvider> future = ProcessCameraProvider.getInstance(this);
        future.addListener(() -> {
            cameraProviderLoading = false;
            if (destroyed || state != State.SCANNING) return;
            try {
                cameraProvider = future.get();
                bindCamera();
            } catch (Exception failure) {
                showError("相机无法启动，请检查相机是否被其他应用占用");
            }
        }, ContextCompat.getMainExecutor(this));
    }

    private void bindCamera() {
        if (cameraProvider == null || previewView == null || state != State.SCANNING
            || !hasCameraPermission()) return;
        try {
            cameraProvider.unbindAll();
            Preview preview = new Preview.Builder().build();
            preview.setSurfaceProvider(previewView.getSurfaceProvider());
            ImageAnalysis analysis = new ImageAnalysis.Builder()
                .setBackpressureStrategy(ImageAnalysis.STRATEGY_KEEP_ONLY_LATEST).build();
            analysis.setAnalyzer(cameraExecutor, this::analyzeFrame);
            cameraProvider.bindToLifecycle(this, CameraSelector.DEFAULT_BACK_CAMERA,
                preview, analysis);
        } catch (Exception failure) {
            showError("相机无法启动，请检查相机是否被其他应用占用");
        }
    }

    private void stopCamera() {
        if (cameraProvider != null) cameraProvider.unbindAll();
    }

    private void analyzeFrame(ImageProxy proxy) {
        if (!active || state != State.SCANNING
            || !frameInFlight.compareAndSet(false, true)) {
            proxy.close();
            return;
        }
        Image image = proxy.getImage();
        if (image == null) {
            frameInFlight.set(false);
            proxy.close();
            return;
        }
        InputImage input = InputImage.fromMediaImage(image,
            proxy.getImageInfo().getRotationDegrees());
        barcodeScanner.process(input).addOnSuccessListener(codes -> {
            if (destroyed || !active || state != State.SCANNING) return;
            for (Barcode barcode : codes) {
                if (acceptQrUri(barcode.getRawValue())) return;
            }
        }).addOnCompleteListener(task -> {
            frameInFlight.set(false);
            proxy.close();
        });
    }

    boolean acceptQrUri(String raw) {
        if (destroyed || !active || state != State.SCANNING) return false;
        String id = DesktopLoginQrUri.requestId(raw);
        if (id.isEmpty()) {
            if (scannerHint != null) scannerHint.setText("请扫描 MaiChat 桌面端登录二维码");
            return false;
        }
        requestId = id;
        state = State.LOADING;
        render();
        stopCamera();
        fetchRequest(id);
        return true;
    }

    private void fetchRequest(String id) {
        io.execute(() -> {
            MaiChatAuthClient.DesktopRequestInfo info = null;
            Exception error = null;
            try { info = authClient.desktopRequestInfo(id); }
            catch (Exception failure) { error = failure; }
            MaiChatAuthClient.DesktopRequestInfo result = info;
            Exception failure = error;
            main.post(() -> {
                if (destroyed || state != State.LOADING || !id.equals(requestId)) return;
                if (failure instanceof MaiChatAuthClient.SessionTerminatedException) {
                    finish();
                    return;
                }
                if (failure != null) {
                    showError(failure.getMessage());
                    return;
                }
                if ("approved".equals(result.status) || "denied".equals(result.status)) {
                    if (!displayCode.isEmpty() && displayCode.equals(result.displayCode)) {
                        approved = "approved".equals(result.status);
                        state = State.RESULT;
                        render();
                    } else showError("该桌面登录请求已处理，请在电脑上刷新二维码");
                    return;
                }
                if (!result.isAwaitingDecision()) {
                    showError("该桌面登录请求已处理或过期，请在电脑上刷新二维码");
                    return;
                }
                platform = result.platform;
                displayCode = result.displayCode;
                expiresAt = SystemClock.elapsedRealtime() + result.expiresIn * 1000L;
                errorMessage = "";
                state = State.CONFIRM;
                render();
            });
        });
    }

    private void decide(boolean approve) {
        if (state != State.CONFIRM) return;
        if (expiresAt <= SystemClock.elapsedRealtime()) {
            showError("二维码已过期，请在电脑上刷新后重新扫码");
            return;
        }
        state = State.DECIDING;
        errorMessage = "";
        render();
        String id = requestId;
        String code = displayCode;
        io.execute(() -> {
            Exception error = null;
            try { authClient.decideDesktopRequest(id, code, approve); }
            catch (Exception failure) { error = failure; }
            Exception failure = error;
            main.post(() -> {
                if (destroyed || state != State.DECIDING || !id.equals(requestId)) return;
                if (failure instanceof MaiChatAuthClient.SessionTerminatedException) {
                    finish();
                    return;
                }
                if (failure != null) {
                    String detail = failure.getMessage() == null ? "操作失败，请重试" :
                        failure.getMessage();
                    if (detail.contains("过期") || detail.contains("已处理")
                        || detail.contains("已变化")) showError(detail);
                    else {
                        errorMessage = detail;
                        state = State.CONFIRM;
                        render();
                    }
                    return;
                }
                approved = approve;
                state = State.RESULT;
                render();
            });
        });
    }

    private void showError(String message) {
        errorMessage = message == null || message.isEmpty() ? "操作失败，请重试" : message;
        state = State.ERROR;
        render();
    }

    private void scanAgain() {
        requestId = "";
        displayCode = "";
        platform = "";
        expiresAt = 0;
        errorMessage = "";
        state = State.SCANNING;
        render();
        ensureCamera();
    }

    private void render() {
        main.removeCallbacks(tick);
        expiryLabel = null;
        previewView = null;
        scannerHint = null;
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(MaiChatTheme.PAGE);
        setContentView(root);
        LinearLayout header = new LinearLayout(this);
        header.setGravity(Gravity.CENTER_VERTICAL);
        header.setPadding(dp(20), 0, dp(20), 0);
        header.setBackgroundColor(Color.WHITE);
        root.addView(header, new LinearLayout.LayoutParams(-1, dp(56)));
        TextView back = MaiChatTheme.label(this, "‹", 32, MaiChatTheme.TEXT);
        back.setGravity(Gravity.CENTER);
        back.setContentDescription("返回设置");
        header.addView(back, new LinearLayout.LayoutParams(dp(40), -1));
        back.setOnClickListener(view -> onBackPressed());
        TextView title = MaiChatTheme.label(this, "扫码登录桌面端", 18, MaiChatTheme.TEXT);
        title.setPadding(dp(8), 0, 0, 0);
        header.addView(title, new LinearLayout.LayoutParams(-1, -1));

        if (state == State.SCANNING) {
            renderScanner(root);
            return;
        }
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        root.addView(scroll, new LinearLayout.LayoutParams(-1, 0, 1));
        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setGravity(Gravity.CENTER);
        content.setPadding(dp(24), dp(24), dp(24), dp(24));
        scroll.addView(content, new FrameLayout.LayoutParams(-1, -2));
        if (state == State.PERMISSION) {
            addTitle(content, "需要相机权限");
            addDetail(content, "允许使用相机，才能扫描电脑上 MaiChat 的登录二维码。");
            addButton(content, "授权相机", true, () -> {
                permissionRequested = true;
                requestPermissions(new String[]{Manifest.permission.CAMERA}, CAMERA_PERMISSION_REQUEST);
            });
            addButton(content, "打开系统设置", false, () -> {
                Intent intent = new Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
                    Uri.fromParts("package", getPackageName(), null));
                startActivity(intent);
            });
        } else if (state == State.LOADING) {
            content.addView(new ProgressBar(this));
            addTitle(content, "正在读取登录请求");
            addDetail(content, "请稍候…");
        } else if (state == State.CONFIRM || state == State.DECIDING) {
            renderConfirmation(content);
        } else if (state == State.RESULT) {
            addTitle(content, approved ? "已批准桌面登录" : "已拒绝桌面登录");
            addDetail(content, approved ? "电脑正在完成登录。" : "该二维码已失效。");
            addButton(content, "返回设置", true, this::finish);
        } else if (state == State.ERROR) {
            addTitle(content, "无法完成扫码登录");
            addDetail(content, errorMessage);
            addButton(content, "重新扫码", true, this::scanAgain);
            addButton(content, "返回设置", false, this::finish);
        }
    }

    private void renderScanner(LinearLayout root) {
        boolean landscape = getResources().getConfiguration().orientation
            == Configuration.ORIENTATION_LANDSCAPE;
        FrameLayout frame = new FrameLayout(this);
        frame.setBackgroundColor(Color.BLACK);
        root.addView(frame, new LinearLayout.LayoutParams(-1, 0, 1));
        previewView = new PreviewView(this);
        previewView.setImplementationMode(PreviewView.ImplementationMode.COMPATIBLE);
        previewView.setScaleType(PreviewView.ScaleType.FILL_CENTER);
        frame.addView(previewView, new FrameLayout.LayoutParams(-1, -1));
        View border = new View(this);
        GradientDrawable outline = MaiChatTheme.rounded(Color.TRANSPARENT, 18, this);
        outline.setStroke(dp(3), Color.WHITE);
        border.setBackground(outline);
        int scanSize = landscape ? dp(180) : dp(248);
        frame.addView(border, new FrameLayout.LayoutParams(scanSize, scanSize, Gravity.CENTER));
        scannerHint = MaiChatTheme.label(this, "将电脑上的登录二维码放入框内", 15, Color.WHITE);
        scannerHint.setGravity(Gravity.CENTER);
        scannerHint.setBackground(MaiChatTheme.rounded(0xCC14213A, 12, this));
        FrameLayout.LayoutParams hintParams = new FrameLayout.LayoutParams(-2, dp(48),
            Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL);
        hintParams.setMargins(dp(20), 0, dp(20), landscape ? dp(8) : dp(48));
        scannerHint.setPadding(dp(16), 0, dp(16), 0);
        frame.addView(scannerHint, hintParams);
    }

    private void renderConfirmation(LinearLayout content) {
        addTitle(content, "确认电脑登录");
        addDetail(content, "请核对电脑上显示的六位数字，完全一致后再批准。");
        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setGravity(Gravity.CENTER);
        card.setPadding(dp(20), dp(22), dp(20), dp(22));
        card.setBackground(MaiChatTheme.bordered(Color.WHITE, MaiChatTheme.BORDER, 16, this));
        LinearLayout.LayoutParams cardParams = new LinearLayout.LayoutParams(-1, -2);
        cardParams.setMargins(0, dp(26), 0, dp(22));
        content.addView(card, cardParams);
        TextView target = MaiChatTheme.label(this,
            platform.equals("mac") ? "Mac 桌面端" : "Windows 桌面端", 16, MaiChatTheme.TEXT);
        target.setGravity(Gravity.CENTER);
        card.addView(target, new LinearLayout.LayoutParams(-1, dp(36)));
        TextView code = MaiChatTheme.label(this, displayCode, 38, MaiChatTheme.BLUE_DARK);
        code.setGravity(Gravity.CENTER);
        code.setLetterSpacing(0.12f);
        code.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        card.addView(code, new LinearLayout.LayoutParams(-1, dp(70)));
        expiryLabel = MaiChatTheme.text(this, "", 13, MaiChatTheme.SECONDARY);
        expiryLabel.setGravity(Gravity.CENTER);
        card.addView(expiryLabel, new LinearLayout.LayoutParams(-1, dp(28)));
        main.post(tick);
        if (!errorMessage.isEmpty()) {
            TextView failure = MaiChatTheme.text(this, errorMessage, 14, MaiChatTheme.RED);
            failure.setGravity(Gravity.CENTER);
            content.addView(failure, new LinearLayout.LayoutParams(-1, dp(48)));
        }
        if (state == State.DECIDING) {
            content.addView(new ProgressBar(this));
            addDetail(content, "正在提交决定…");
        } else {
            addButton(content, "数字一致，批准登录", true, () -> decide(true));
            addButton(content, "拒绝登录", false, () -> decide(false));
        }
    }

    private void addTitle(LinearLayout content, String value) {
        TextView title = MaiChatTheme.label(this, value, 22, MaiChatTheme.TEXT);
        title.setGravity(Gravity.CENTER);
        content.addView(title, new LinearLayout.LayoutParams(-1, dp(50)));
    }

    private void addDetail(LinearLayout content, String value) {
        TextView detail = MaiChatTheme.text(this, value, 15, MaiChatTheme.SECONDARY);
        detail.setGravity(Gravity.CENTER);
        detail.setLineSpacing(dp(3), 1);
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(-1, -2);
        params.setMargins(0, dp(8), 0, dp(12));
        content.addView(detail, params);
    }

    private void addButton(LinearLayout content, String title, boolean primary, Runnable action) {
        Button button = new Button(this);
        button.setAllCaps(false);
        button.setText(title);
        button.setTextSize(16);
        button.setTextColor(primary ? Color.WHITE : MaiChatTheme.TEXT);
        button.setBackground(primary
            ? MaiChatTheme.rounded(MaiChatTheme.BLUE, 10, this)
            : MaiChatTheme.bordered(Color.WHITE, MaiChatTheme.BORDER, 10, this));
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(-1, dp(52));
        params.setMargins(0, dp(12), 0, 0);
        content.addView(button, params);
        button.setOnClickListener(view -> action.run());
    }

    private int dp(float value) { return MaiChatTheme.dp(this, value); }
}
