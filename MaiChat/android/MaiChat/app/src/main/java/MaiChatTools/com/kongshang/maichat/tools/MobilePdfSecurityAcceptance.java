// 文件作用：显式 Debug Intent 下在正常 App 验收原有 Android 排版→私有表单→共用加密核心。
// 默认关闭，不注册成工具，不自动修改账号/发送消息；仅用户点击安全测试按钮才运行。
package com.kongshang.maichat.tools;

import android.view.Gravity;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.FrameLayout;
import com.kongshang.maichat.BuildConfig;
import com.kongshang.maichat.MainActivity;
import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.UUID;
import java.util.concurrent.CancellationException;
import java.util.concurrent.ExecutorService;
import org.json.JSONObject;

public final class MobilePdfSecurityAcceptance {
    private MobilePdfSecurityAcceptance() {}

    public static Button installIfRequested(MainActivity activity, MobilePdfPasswordRequests prompts,
        ExecutorService worker) {
        if (!BuildConfig.DEBUG || !activity.getIntent().getBooleanExtra("maichat-pdf-security-ui-test", false))
            return null;
        Button run = new Button(activity);
        run.setText("PDF 本地验收");
        run.setContentDescription("pdf-security-test-run");
        FrameLayout.LayoutParams layout = new FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT,
            Gravity.TOP | Gravity.END);
        layout.topMargin = Math.round(46 * activity.getResources().getDisplayMetrics().density);
        activity.addContentView(run, layout);
        run.setOnClickListener(view -> {
            run.setEnabled(false);
            worker.execute(() -> {
                File directory = new File(activity.getCacheDir(), "pdf-security-acceptance/" + UUID.randomUUID());
                File source = new File(directory, "source.pdf"), output = new File(directory, "protected.pdf");
                JSONObject proof = new JSONObject();
                String stage = "prepare";
                try {
                    if (!directory.mkdirs()) throw new java.io.IOException("Test directory unavailable");
                    stage = "render";
                    JSONObject rendered = AgentPdfRenderer.render(
                        "<html><body><h1>MaiChat PDF</h1><p>private fixture content</p></body></html>", source);
                    stage = "password-form";
                    String secret = prompts.requestPassword();
                    if (secret == null) throw new CancellationException();
                    stage = "encrypt-response";
                    JSONObject encrypted = MaiPdfSecurity.encrypt(source, output, secret, false);
                    stage = "response-privacy";
                    if (encrypted.toString().contains(secret)) throw new java.io.IOException("Unsafe response");
                    stage = "encrypted-marker";
                    String bytes = new String(Files.readAllBytes(output.toPath()), StandardCharsets.ISO_8859_1);
                    if (!bytes.startsWith("%PDF-") || !bytes.contains("/Encrypt"))
                        throw new java.io.IOException("Encrypted output unavailable");
                    stage = "proof-metadata";
                    proof.put("status", "passed").put("source_pages", rendered.getInt("pages"))
                        .put("page_count", encrypted.getLong("page_count"))
                        .put("password_protected", true).put("path", output.getAbsolutePath());
                } catch (CancellationException | InterruptedException canceled) {
                    try { proof.put("status", "canceled").put("output_exists", output.exists()); }
                    catch (Exception ignored) {}
                    if (canceled instanceof InterruptedException) Thread.currentThread().interrupt();
                } catch (Exception failure) {
                    try { proof.put("status", "failed").put("stage", stage)
                        .put("failure_kind", failure.getClass().getSimpleName()); } catch (Exception ignored) {}
                } finally {
                    source.delete();
                    // 只保存无凭据证明；加密文件供独立阅读器检查，明文输入在每条路径均清理。
                    try (FileOutputStream file = new FileOutputStream(new File(directory, "result.json"))) {
                        file.write(proof.toString().getBytes(StandardCharsets.UTF_8));
                    } catch (Exception ignored) {}
                    String value = proof.toString();
                    activity.runOnUiThread(() -> {
                        run.setText("PDF " + (value.contains("\"passed\"") ? "通过" :
                            value.contains("\"canceled\"") ? "已取消" : "失败"));
                        run.setContentDescription("pdf-security-test-result " + value);
                        run.setEnabled(true);
                    });
                }
            });
        });
        return run;
    }

    /** 正常 render 会替换根容器；重新附着同一个测试按钮，保留在途状态而不是创建重复入口。 */
    public static void reattach(MainActivity activity, Button button) {
        if (button == null) return;
        android.view.ViewGroup.LayoutParams layout = button.getLayoutParams();
        if (button.getParent() instanceof ViewGroup) ((ViewGroup) button.getParent()).removeView(button);
        activity.addContentView(button, layout);
    }
}
