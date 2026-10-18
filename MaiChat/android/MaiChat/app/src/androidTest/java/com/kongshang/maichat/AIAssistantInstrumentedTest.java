// 文件作用：Android 设备集成测试。
// AIAssistantInstrumentedTest 覆盖对应界面或服务的设备行为。
// 运行边界：仅供自动化验证使用，不作为正式 App 或 Agent 的用户功能入口。
// 覆盖场景：bundledRvmProcessesVideoWithOriginalAudio、streamsUnicodeStopsAndKeepsNativeComposer、
// httpsUsesSystemRootsAndRejectsExpiredCertificate、visibleText。
package com.kongshang.maichat;

import static org.junit.Assert.*;

import android.content.ClipData;
import android.content.Context;
import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.Color;
import android.net.Uri;
import android.view.View;
import android.view.ViewGroup;
import android.view.accessibility.AccessibilityNodeInfo;
import android.widget.TextView;
import androidx.core.content.FileProvider;
import androidx.test.core.app.ActivityScenario;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Files;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.UUID;
import org.json.JSONObject;
import org.junit.Test;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
public class AIAssistantInstrumentedTest {
    @Test
    public void bundledRvmProcessesVideoWithOriginalAudio() throws Exception {
        Context app = InstrumentationRegistry.getInstrumentation().getTargetContext();
        Context tests = InstrumentationRegistry.getInstrumentation().getContext();
        File workspace = new File(app.getCacheDir(), "RvmIntegration-" + UUID.randomUUID());
        assertTrue(workspace.mkdirs());
        File input = new File(workspace, "sample.mp4");
        File model = new File(workspace, "rvm_mobilenetv3_fp32.onnx");
        File output = new File(workspace, "result.mp4");
        try {
            try (InputStream source = tests.getAssets().open("gallery-sample.mp4")) {
                Files.copy(source, input.toPath());
            }
            try (InputStream source = app.getAssets().open(
                     "MaiAgentModels/rvm_mobilenetv3_fp32.onnx")) {
                Files.copy(source, model.toPath());
            }
            File runtime = new File(app.getApplicationInfo().nativeLibraryDir,
                "libonnxruntime.so");
            assertTrue("ONNX Runtime must be packaged in the app", runtime.isFile());
            System.loadLibrary("maichat_agent");
            JSONObject request = new JSONObject()
                .put("input_path", input.getAbsolutePath())
                .put("output_path", "result.mp4")
                .put("background", new JSONObject().put("mode", "solid")
                    .put("color", "#008040"));
            byte[] response = AIAssistantController.nativeMatteVideo(
                request.toString().getBytes(StandardCharsets.UTF_8),
                workspace.getAbsolutePath().getBytes(StandardCharsets.UTF_8),
                model.getAbsolutePath().getBytes(StandardCharsets.UTF_8),
                runtime.getAbsolutePath().getBytes(StandardCharsets.UTF_8));
            assertNotNull("Native video matting returned no response", response);
            JSONObject result = new JSONObject(new String(response, StandardCharsets.UTF_8));
            assertTrue(result.toString(), result.optBoolean("ok"));
            JSONObject details = new JSONObject(result.getString("output"));
            assertTrue(details.toString(), details.getInt("frames") > 1);
            assertEquals(1, details.getInt("audio_streams_copied"));
            assertTrue(output.isFile() && output.length() > 0);
            assertTrue(input.isFile() && input.length() > 0);
        } finally {
            Files.deleteIfExists(output.toPath());
            Files.deleteIfExists(input.toPath());
            Files.deleteIfExists(model.toPath());
            Files.deleteIfExists(workspace.toPath());
        }
    }
    private interface Check {
        boolean check(AIAssistantPanel panel) throws Exception;
    }
    private void await(ActivityScenario<AIAssistantTestActivity> scenario, Check check) throws Exception {
        long deadline = System.currentTimeMillis() + 15000;
        AtomicBoolean passed = new AtomicBoolean();
        while (!passed.get() && System.currentTimeMillis() < deadline) {
            dismissPageSizeCompatibilityDialog();
            scenario.onActivity(a -> {
                try {
                    passed.set(check.check(a.panel));
                } catch (Exception e) {
                    throw new AssertionError(e);
                }
            });
            Thread.sleep(50);
        }
        assertTrue("AI UI condition timed out", passed.get());
    }
    private void dismissPageSizeCompatibilityDialog() {
        AccessibilityNodeInfo root = InstrumentationRegistry.getInstrumentation()
            .getUiAutomation().getRootInActiveWindow();
        if (root == null) return;
        for (String label : new String[] {"Don't Show Again", "OK"}) {
            java.util.List<AccessibilityNodeInfo> matches = root.findAccessibilityNodeInfosByText(label);
            if (!matches.isEmpty()) {
                matches.get(0).performAction(AccessibilityNodeInfo.ACTION_CLICK);
                return;
            }
        }
    }
    private AccessibilityNodeInfo awaitSystemText(String text, long timeoutMilliseconds)
            throws InterruptedException {
        long deadline = System.currentTimeMillis() + timeoutMilliseconds;
        while (System.currentTimeMillis() < deadline) {
            AccessibilityNodeInfo root = InstrumentationRegistry.getInstrumentation()
                .getUiAutomation().getRootInActiveWindow();
            if (root != null) {
                java.util.List<AccessibilityNodeInfo> matches = root.findAccessibilityNodeInfosByText(text);
                if (!matches.isEmpty()) return matches.get(0);
            }
            Thread.sleep(50);
        }
        return null;
    }
    private boolean clickNodeOrParent(AccessibilityNodeInfo node) {
        AccessibilityNodeInfo current = node;
        while (current != null) {
            if (current.performAction(AccessibilityNodeInfo.ACTION_CLICK)) return true;
            current = current.getParent();
        }
        return false;
    }
    private boolean visibleText(View view, String text) {
        if (view instanceof TextView && ((TextView) view).getText().toString().contains(text)) {
            android.graphics.Rect bounds = new android.graphics.Rect();
            return view.getGlobalVisibleRect(bounds) && bounds.height() > 0;
        }
        if (view instanceof ViewGroup)
            for (int i = 0; i < ((ViewGroup) view).getChildCount(); i++)
                if (visibleText(((ViewGroup) view).getChildAt(i), text))
                    return true;
        return false;
    }
    private View findDescription(View view, String description) {
        if (description.contentEquals(
                view.getContentDescription() == null ? "" : view.getContentDescription()))
            return view;
        if (view instanceof ViewGroup)
            for (int i = 0; i < ((ViewGroup) view).getChildCount(); i++) {
                View result = findDescription(((ViewGroup) view).getChildAt(i), description);
                if (result != null)
                    return result;
            }
        return null;
    }
    private View findList(View view) {
        if (view instanceof android.widget.ListView)
            return view;
        if (view instanceof ViewGroup)
            for (int i = 0; i < ((ViewGroup) view).getChildCount(); i++) {
                View result = findList(((ViewGroup) view).getChildAt(i));
                if (result != null)
                    return result;
            }
        return null;
    }

    @Test public void tappingConversationDismissesComposerFocusAndKeepsDraft() {
        Context app = InstrumentationRegistry.getInstrumentation().getTargetContext();
        Intent launch = new Intent(app, AIAssistantTestActivity.class)
            .putExtra("model_url", "http://127.0.0.1:18190");
        try (ActivityScenario<AIAssistantTestActivity> scenario = ActivityScenario.launch(launch)) {
            scenario.onActivity(activity -> {
                AIAssistantPanel panel = activity.panel;
                panel.composer.setText("draft stays");
                panel.composer.requestFocus();
                assertTrue(panel.composer.hasFocus());
                View history = findList(panel);
                assertNotNull(history);
                assertTrue(history.getHeight() > 0);
                float x = history.getWidth() / 2f, y = history.getHeight() / 2f;
                long now = android.os.SystemClock.uptimeMillis();
                android.view.MotionEvent down = android.view.MotionEvent.obtain(now, now,
                    android.view.MotionEvent.ACTION_DOWN, x, y, 0);
                android.view.MotionEvent up = android.view.MotionEvent.obtain(now, now + 40,
                    android.view.MotionEvent.ACTION_UP, x, y, 0);
                try {
                    history.dispatchTouchEvent(down);
                    history.dispatchTouchEvent(up);
                } finally { down.recycle(); up.recycle(); }
                assertFalse("Blank conversation tap must release keyboard focus", panel.composer.hasFocus());
                assertEquals("draft stays", panel.composer.getText().toString());
            });
        }
    }

    @Test public void keyboardSendActionSubmitsTheDraft() throws Exception {
        Context app = InstrumentationRegistry.getInstrumentation().getTargetContext();
        Intent launch = new Intent(app, AIAssistantTestActivity.class)
            .putExtra("model_url", "http://127.0.0.1:18190");
        try (ActivityScenario<AIAssistantTestActivity> scenario = ActivityScenario.launch(launch)) {
            await(scenario, panel -> panel.controller.state.ready);
            scenario.onActivity(activity -> {
                activity.panel.composer.setText("keyboard send check");
                activity.panel.composer.onEditorAction(
                    android.view.inputmethod.EditorInfo.IME_ACTION_SEND);
            });
            await(scenario, panel -> visibleText(panel, "keyboard send check") &&
                panel.composer.getText().length() == 0);
        }
    }

    private boolean hasQuotePart(AIAssistantController.State state) {
        org.json.JSONArray messages = state.data.optJSONArray("messages");
        if (messages == null) return false;
        for (int i = 0; i < messages.length(); i++) {
            JSONObject message = messages.optJSONObject(i);
            if (message == null || !message.optString("role").equals("user")) continue;
            org.json.JSONArray parts = message.optJSONArray("parts");
            if (parts == null) continue;
            for (int j = 0; j < parts.length(); j++) {
                JSONObject part = parts.optJSONObject(j);
                if (part != null && part.optString("kind").equals("quote")) return true;
            }
        }
        return false;
    }

    private static void writeSilentWav(File file) throws Exception {
        int dataBytes = 16000 * 2 * 3;
        ByteBuffer header = ByteBuffer.allocate(44).order(ByteOrder.LITTLE_ENDIAN);
        header.put("RIFF".getBytes(StandardCharsets.US_ASCII)).putInt(36 + dataBytes);
        header.put("WAVEfmt ".getBytes(StandardCharsets.US_ASCII)).putInt(16);
        header.putShort((short) 1).putShort((short) 1);
        header.putInt(16000).putInt(32000).putShort((short) 2).putShort((short) 16);
        header.put("data".getBytes(StandardCharsets.US_ASCII)).putInt(dataBytes);
        try (FileOutputStream output = new FileOutputStream(file)) {
            output.write(header.array());
            output.write(new byte[dataBytes]);
        }
    }

    @Test
    public void audioArtifactUsesIosStyleCardAndPlays() throws Exception {
        Context app = InstrumentationRegistry.getInstrumentation().getTargetContext();
        Intent launch = new Intent(app, AIAssistantTestActivity.class)
            .putExtra("model_url", "http://127.0.0.1:18190");
        try (ActivityScenario<AIAssistantTestActivity> scenario = ActivityScenario.launch(launch)) {
            await(scenario, panel -> panel.controller.state.ready);
            java.util.concurrent.atomic.AtomicReference<File> path =
                new java.util.concurrent.atomic.AtomicReference<>();
            scenario.onActivity(activity ->
                path.set(activity.panel.controller.workspaceFile("parity-audio.wav")));
            assertNotNull(path.get());
            try {
                writeSilentWav(path.get());
                scenario.onActivity(activity -> {
                    activity.panel.composer.setText("audio card");
                    assertTrue(activity.panel.send.performClick());
                });
                await(scenario, panel -> !panel.controller.state.busy() &&
                    findDescription(panel, "播放音频") != null && visibleText(panel, "0:03"));
                scenario.onActivity(activity -> {
                    View card = findDescription(activity.panel, "播放音频");
                    assertNotNull(card);
                    assertEquals(Math.round(240 * activity.getResources().getDisplayMetrics().density),
                        card.getLayoutParams().width);
                });
                Bitmap screenshot = InstrumentationRegistry.getInstrumentation()
                    .getUiAutomation().takeScreenshot();
                assertNotNull(screenshot);
                File output = new File(app.getExternalFilesDir(null), "android-agent-audio.png");
                try (FileOutputStream stream = new FileOutputStream(output)) {
                    assertTrue(screenshot.compress(Bitmap.CompressFormat.PNG, 100, stream));
                } finally { screenshot.recycle(); }
                scenario.onActivity(activity ->
                    assertTrue(findDescription(activity.panel, "播放音频").performClick()));
                await(scenario, panel -> findDescription(panel, "暂停音频") != null);
            } finally { Files.deleteIfExists(path.get().toPath()); }
        }
    }

    @Test
    public void quotedMessageAppearsInComposerAndSentHistory() throws Exception {
        Context app = InstrumentationRegistry.getInstrumentation().getTargetContext();
        Intent launch = new Intent(app, AIAssistantTestActivity.class)
            .putExtra("model_url", "http://127.0.0.1:18190");
        try (ActivityScenario<AIAssistantTestActivity> scenario = ActivityScenario.launch(launch)) {
            await(scenario, panel -> panel.controller.state.ready);
            scenario.onActivity(activity -> {
                activity.panel.composer.setText("first");
                assertTrue(activity.panel.send.performClick());
            });
            await(scenario, panel -> !panel.controller.state.busy() &&
                visibleText(panel, "Mobile response ready"));
            scenario.onActivity(activity -> {
                View quote = findDescription(activity.panel, "引用这条消息");
                assertNotNull(quote);
                assertTrue(quote.performClick());
            });
            await(scenario, panel -> {
                View cancel = findDescription(panel, "取消引用");
                return cancel != null && cancel.isShown() &&
                    (visibleText(panel, "引用你的消息") || visibleText(panel, "引用 AI 消息"));
            });
            Bitmap screenshot = InstrumentationRegistry.getInstrumentation()
                .getUiAutomation().takeScreenshot();
            assertNotNull(screenshot);
            File output = new File(app.getExternalFilesDir(null), "android-agent-quote.png");
            try (FileOutputStream stream = new FileOutputStream(output)) {
                assertTrue(screenshot.compress(Bitmap.CompressFormat.PNG, 100, stream));
            } finally { screenshot.recycle(); }
            scenario.onActivity(activity -> {
                activity.panel.composer.setText("second");
                assertTrue(activity.panel.send.performClick());
            });
            await(scenario, panel -> hasQuotePart(panel.controller.state));
            await(scenario, panel -> visibleText(panel, "↩  first"));
            scenario.onActivity(activity -> {
                View cancel = findDescription(activity.panel, "取消引用");
                assertNotNull(cancel);
                assertEquals(View.GONE, cancel.getParent() instanceof View
                    ? ((View) cancel.getParent()).getVisibility() : View.VISIBLE);
            });
        }
    }

    @Test
    public void mediaToolsCollapseToOneLineAndVideoCardsMatchIos() throws Exception {
        Context app = InstrumentationRegistry.getInstrumentation().getTargetContext();
        Context tests = InstrumentationRegistry.getInstrumentation().getContext();
        Intent launch = new Intent(app, AIAssistantTestActivity.class)
            .putExtra("model_url", "http://127.0.0.1:18190");
        try (ActivityScenario<AIAssistantTestActivity> scenario = ActivityScenario.launch(launch)) {
            await(scenario, panel -> panel.controller.state.ready);
            File first = null, second = null;
            try {
                java.util.concurrent.atomic.AtomicReference<File> firstPath =
                    new java.util.concurrent.atomic.AtomicReference<>();
                java.util.concurrent.atomic.AtomicReference<File> secondPath =
                    new java.util.concurrent.atomic.AtomicReference<>();
                scenario.onActivity(activity -> {
                    firstPath.set(activity.panel.controller.workspaceFile("parity-one.mp4"));
                    secondPath.set(activity.panel.controller.workspaceFile("parity-two.mp4"));
                });
                first = firstPath.get(); second = secondPath.get();
                assertNotNull(first); assertNotNull(second);
                for (File target : new File[] {first, second})
                    try (InputStream source = tests.getAssets().open("gallery-sample.mp4")) {
                        Files.copy(source, target.toPath());
                    }
                scenario.onActivity(activity -> {
                    activity.panel.composer.setText("media cards");
                    assertTrue(activity.panel.send.performClick());
                });
                await(scenario, panel -> !panel.controller.state.busy() &&
                    panel.controller.state.data.toString().contains("parity-two.mp4"));
                scenario.onActivity(activity -> {
                    android.widget.ListView list = (android.widget.ListView) findList(activity.panel);
                    assertNotNull(list);
                    list.setSelection(0);
                });
                await(scenario, panel -> findDescription(panel,
                    "工具调用 1 次，已完成 · system_resources ×1") != null);
                scenario.onActivity(activity -> {
                    View group = findDescription(activity.panel,
                        "工具调用 1 次，已完成 · system_resources ×1");
                    assertTrue(group instanceof TextView);
                    assertEquals(1, ((TextView) group).getMaxLines());
                    assertNotNull(findDescription(activity.panel, "播放视频 parity-one.mp4"));
                    assertNotNull(findDescription(activity.panel, "播放视频 parity-two.mp4"));
                });
                Bitmap screenshot = InstrumentationRegistry.getInstrumentation()
                    .getUiAutomation().takeScreenshot();
                assertNotNull(screenshot);
                File output = new File(app.getExternalFilesDir(null), "android-agent-parity.png");
                try (FileOutputStream stream = new FileOutputStream(output)) {
                    assertTrue(screenshot.compress(Bitmap.CompressFormat.PNG, 100, stream));
                } finally { screenshot.recycle(); }
            } finally {
                if (first != null) Files.deleteIfExists(first.toPath());
                if (second != null) Files.deleteIfExists(second.toPath());
            }
        }
    }
    @Test
    // 验证混合媒体先留在草稿、可移除，且切换会话不会丢失说明和附件。
    public void mediaDraftWaitsForSendAndStaysInItsSession() throws Exception {
        Context app = InstrumentationRegistry.getInstrumentation().getTargetContext();
        Context tests = InstrumentationRegistry.getInstrumentation().getContext();
        File sources = new File(app.getCacheDir(), "AssistantMediaDraft-" + UUID.randomUUID());
        assertTrue(sources.mkdirs());
        File image = new File(sources, "sample.png");
        File video = new File(sources, "sample.mp4");
        Bitmap pixels = Bitmap.createBitmap(16, 16, Bitmap.Config.ARGB_8888);
        pixels.eraseColor(Color.BLUE);
        try {
            try (FileOutputStream output = new FileOutputStream(image)) {
                assertTrue(pixels.compress(Bitmap.CompressFormat.PNG, 100, output));
            }
            try (InputStream input = tests.getAssets().open("gallery-sample.mp4")) {
                Files.copy(input, video.toPath());
            }
            Uri imageUri = FileProvider.getUriForFile(app,
                app.getPackageName() + ".files", image);
            Uri videoUri = FileProvider.getUriForFile(app,
                app.getPackageName() + ".files", video);
            Intent selection = new Intent();
            ClipData items = ClipData.newUri(app.getContentResolver(), "media", imageUri);
            items.addItem(new ClipData.Item(videoUri));
            selection.setClipData(items);
            try (ActivityScenario<AIAssistantTestActivity> scenario =
                     ActivityScenario.launch(AIAssistantTestActivity.class)) {
                await(scenario, panel -> panel.controller.state.ready);
                scenario.onActivity(activity -> activity.panel.controller.select(""));
                await(scenario, panel -> panel.controller.state.selected.isEmpty());
                InstrumentationRegistry.getInstrumentation().waitForIdleSync();
                java.util.concurrent.atomic.AtomicReference<String> origin =
                    new java.util.concurrent.atomic.AtomicReference<>();
                java.util.concurrent.atomic.AtomicInteger initialCount =
                    new java.util.concurrent.atomic.AtomicInteger();
                scenario.onActivity(activity -> {
                    AIAssistantPanel panel = activity.panel;
                    origin.set(panel.controller.state.selected);
                    org.json.JSONArray messages = panel.controller.state.data.optJSONArray("messages");
                    initialCount.set(messages == null ? 0 : messages.length());
                    panel.composer.setText("请分析这些素材");
                    panel.handleActivityResult(AIAssistantPanel.REQUEST_IMAGE, selection);
                });
                assertEquals("", origin.get());
                await(scenario, panel -> visibleText(panel, "图片 1")
                    && visibleText(panel, "视频 1") && panel.send.isEnabled());
                File captures = new File(app.getExternalFilesDir(null), "test-captures");
                assertTrue(captures.isDirectory() || captures.mkdirs());
                Bitmap mixed = InstrumentationRegistry.getInstrumentation()
                    .getUiAutomation().takeScreenshot();
                try (FileOutputStream output = new FileOutputStream(
                         new File(captures, "android-ai-media-mixed.png"))) {
                    assertTrue(mixed.compress(Bitmap.CompressFormat.PNG, 100, output));
                }
                scenario.onActivity(activity -> {
                    activity.panel.composer.requestFocus();
                    ((android.view.inputmethod.InputMethodManager) activity.getSystemService(
                         Context.INPUT_METHOD_SERVICE)).showSoftInput(
                             activity.panel.composer,
                             android.view.inputmethod.InputMethodManager.SHOW_IMPLICIT);
                });
                await(scenario, panel -> {
                    androidx.core.view.WindowInsetsCompat insets =
                        androidx.core.view.ViewCompat.getRootWindowInsets(panel);
                    return insets != null
                        && insets.isVisible(androidx.core.view.WindowInsetsCompat.Type.ime());
                });
                Thread.sleep(400);
                Bitmap withKeyboard = InstrumentationRegistry.getInstrumentation()
                    .getUiAutomation().takeScreenshot();
                try (FileOutputStream output = new FileOutputStream(
                         new File(captures, "android-ai-media-keyboard.png"))) {
                    assertTrue(withKeyboard.compress(Bitmap.CompressFormat.PNG, 100, output));
                }
                scenario.onActivity(activity -> {
                    AIAssistantPanel panel = activity.panel;
                    org.json.JSONArray messages = panel.controller.state.data.optJSONArray("messages");
                    assertEquals(initialCount.get(), messages == null ? 0 : messages.length());
                    assertEquals("请分析这些素材", panel.composer.getText().toString());
                    assertNotNull(findDescription(panel, "预览视频 1"));
                    assertTrue(findDescription(panel, "对话列表").performClick());
                });
                AccessibilityNodeInfo newConversation = awaitSystemText("新对话", 3000);
                assertNotNull("会话抽屉应提供新对话入口", newConversation);
                assertTrue(clickNodeOrParent(newConversation));
                await(scenario, panel -> !panel.controller.state.selected.equals(origin.get())
                    && panel.composer.getText().length() == 0
                    && findDescription(panel, "预览图片") == null);
                if (origin.get().isEmpty()) {
                    scenario.onActivity(activity ->
                        assertTrue(findDescription(activity.panel, "对话列表").performClick()));
                    AccessibilityNodeInfo draft = awaitSystemText("未发送草稿", 3000);
                    assertNotNull("新对话后应能返回未发送草稿", draft);
                    assertTrue("未发送草稿应可点击", clickNodeOrParent(draft));
                } else {
                    scenario.onActivity(activity -> activity.panel.controller.select(origin.get()));
                }
                await(scenario, panel -> panel.controller.state.selected.equals(origin.get())
                    && panel.composer.getText().toString().equals("请分析这些素材")
                    && findDescription(panel, "预览图片") != null
                    && findDescription(panel, "预览视频 1") != null);
                scenario.onActivity(activity ->
                    assertTrue(findDescription(activity.panel, "移除视频").performClick()));
                await(scenario, panel -> findDescription(panel, "预览图片") != null
                    && findDescription(panel, "预览视频 1") == null);
                Bitmap screenshot = InstrumentationRegistry.getInstrumentation()
                    .getUiAutomation().takeScreenshot();
                try (FileOutputStream output = new FileOutputStream(
                         new File(captures, "android-ai-media-draft.png"))) {
                    assertTrue(screenshot.compress(Bitmap.CompressFormat.PNG, 100, output));
                }
            }
        } finally {
            pixels.recycle();
            Files.deleteIfExists(image.toPath());
            Files.deleteIfExists(video.toPath());
            Files.deleteIfExists(sources.toPath());
        }
    }
    @Test
    // 验证创作中心主界面与 iOS 共用的入口层级和输入区状态切换。
    public void composerMatchesIosInteractionStructure() throws Exception {
        try (ActivityScenario<AIAssistantTestActivity> scenario =
                 ActivityScenario.launch(AIAssistantTestActivity.class)) {
            await(scenario, panel -> panel.controller.state.ready);
            scenario.onActivity(activity -> {
                AIAssistantPanel panel = activity.panel;
                assertNotNull(findDescription(panel, "对话列表"));
                assertNotNull(findDescription(panel, "更多"));
                assertNull(findDescription(panel, "新对话"));
                assertNull(findDescription(panel, "权限设置"));
                assertNull(findDescription(panel, "切换模型"));
                assertNotNull(findDescription(panel, "切换到按住转文字"));
                assertNotNull(findDescription(panel, "展开更多功能"));
                assertEquals(View.GONE, panel.send.getVisibility());
                panel.composer.setText("hello");
                assertEquals(View.VISIBLE, panel.send.getVisibility());
                assertNotNull(findDescription(panel, "展开更多功能"));
                panel.composer.setText("");
                assertTrue(findDescription(panel, "展开更多功能").performClick());
                assertNotNull(findDescription(panel, "选择图片或视频"));
                assertNotNull(findDescription(panel, "拍照"));
                assertNotNull(findDescription(panel, "导入文本文件"));
                assertTrue(findDescription(panel, "切换到按住转文字").performClick());
                assertNotNull(findDescription(panel, "按住转文字"));
                assertNotNull(findDescription(panel, "切换到键盘输入"));
            });
        }
    }
    @Test
    // 作用：验证 Unicode 增量输出、停止动作及原生输入框状态在同一会话中保持一致。
    public void streamsUnicodeStopsAndKeepsNativeComposer() throws Exception {
        try (ActivityScenario<AIAssistantTestActivity> scenario =
                 ActivityScenario.launch(AIAssistantTestActivity.class)) {
            await(scenario, p -> p.controller.state.ready);
            java.util.concurrent.atomic.AtomicReference<android.widget.EditText> input =
                new java.util.concurrent.atomic.AtomicReference<>();
            scenario.onActivity(a -> {
                input.set(a.panel.composer);
                a.panel.composer.setText("中文🙂 UI streaming check");
                assertEquals(View.VISIBLE, a.panel.send.getVisibility());
                assertTrue(a.panel.send.isEnabled());
                assertTrue(a.panel.send.performClick());
            });
            await(scenario, p -> !p.controller.state.busy() && visibleText(p, "Mobile response ready"));
            scenario.onActivity(a -> {
                assertSame(input.get(), a.panel.composer);
                assertEquals("", a.panel.composer.getText().toString());
            });
            await(scenario, p -> {
                org.json.JSONArray messages = p.controller.state.data.getJSONArray("messages");
                return messages.getJSONObject(0)
                    .getJSONArray("parts")
                    .getJSONObject(0)
                    .getString("text")
                    .equals("中文🙂 UI streaming check");
            });
            scenario.onActivity(a -> {
                a.panel.composer.setText("write");
                assertEquals(View.VISIBLE, a.panel.send.getVisibility());
                assertTrue(a.panel.send.isEnabled());
                assertTrue(a.panel.send.performClick());
            });
            await(scenario,
                p
                -> p.controller.state.data.optJSONArray("permissions") != null
                    && p.controller.state.data.optJSONArray("permissions").length() == 1);
            scenario.onActivity(a -> {
                View allow = findDescription(a.panel, "允许一次");
                assertNotNull(allow);
                assertTrue(allow.performClick());
            });
            await(scenario,
                p
                -> !p.controller.state.busy()
                    && p.controller.state.data.optJSONArray("permissions").length() == 0);
            scenario.onActivity(a -> {
                a.panel.composer.setText("stop");
                assertTrue(a.panel.send.isEnabled());
                assertTrue(a.panel.send.performClick());
            });
            await(scenario, p -> p.controller.state.busy());
            scenario.onActivity(a -> {
                View stop = findDescription(a.panel, "停止");
                assertNotNull(stop);
                assertEquals(View.GONE, a.panel.send.getVisibility());
                assertTrue(stop.performClick());
            });
            await(scenario, p -> !p.controller.state.busy());
            scenario.onActivity(a -> {
                a.panel.composer.requestFocus();
                ((android.view.inputmethod.InputMethodManager) a.getSystemService(
                     Context.INPUT_METHOD_SERVICE))
                    .showSoftInput(
                        a.panel.composer, android.view.inputmethod.InputMethodManager.SHOW_IMPLICIT);
            });
            await(scenario, p -> {
                androidx.core.view.WindowInsetsCompat insets =
                    androidx.core.view.ViewCompat.getRootWindowInsets(p);
                return insets != null && insets.isVisible(androidx.core.view.WindowInsetsCompat.Type.ime());
            });
            scenario.onActivity(a -> {
                a.panel.composer.setText("long");
                assertTrue(a.panel.send.isEnabled());
                assertTrue(a.panel.send.performClick());
            });
            await(scenario, p -> !p.controller.state.busy() && visibleText(p, "Tail marker"));
            java.util.concurrent.atomic.AtomicReference<String> latestPosition =
                new java.util.concurrent.atomic.AtomicReference<>("not measured");
            try {
                await(scenario, p -> {
                    android.widget.ListView list = (android.widget.ListView) findList(p);
                    View last = list == null || list.getChildCount() == 0 ? null
                        : list.getChildAt(list.getChildCount() - 1);
                    androidx.core.view.WindowInsetsCompat insets =
                        androidx.core.view.ViewCompat.getRootWindowInsets(p);
                    boolean imeVisible = insets != null
                        && insets.isVisible(androidx.core.view.WindowInsetsCompat.Type.ime());
                    int visibleBottom = list == null ? -1 : list.getHeight()
                        - (list.getClipToPadding() ? list.getPaddingBottom() : 0);
                    latestPosition.set("count=" + (list == null ? -1 : list.getCount())
                        + ", firstVisible=" + (list == null ? -1 : list.getFirstVisiblePosition())
                        + ", lastVisible=" + (list == null ? -1 : list.getLastVisiblePosition())
                        + ", listHeight=" + (list == null ? -1 : list.getHeight())
                        + ", paddingBottom=" + (list == null ? -1 : list.getPaddingBottom())
                        + ", clipToPadding=" + (list != null && list.getClipToPadding())
                        + ", visibleBottom=" + visibleBottom
                        + ", lastTop=" + (last == null ? -1 : last.getTop())
                        + ", lastBottom=" + (last == null ? -1 : last.getBottom())
                        + ", lastHeight=" + (last == null ? -1 : last.getHeight())
                        + ", imeVisible=" + imeVisible);
                    return list != null && list.getLastVisiblePosition() == list.getCount() - 1
                        && last != null
                        && last.getBottom() <= visibleBottom + 2
                        && last.getBottom() > 0 && imeVisible;
                });
            } catch (AssertionError error) {
                throw new AssertionError("Latest message geometry: " + latestPosition.get(), error);
            }
            CountDownLatch saved = new CountDownLatch(1);
            AtomicBoolean savedOK = new AtomicBoolean();
            scenario.onActivity(a
                -> a.panel.controller.save("glm-5.3", "on-request", ok -> {
                        savedOK.set(ok);
                        saved.countDown();
                    }));
            assertTrue(saved.await(8, TimeUnit.SECONDS));
            assertTrue(savedOK.get());
            Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
            Bitmap screenshot =
                InstrumentationRegistry.getInstrumentation().getUiAutomation().takeScreenshot();
            File captures = new File(context.getExternalFilesDir(null), "test-captures");
            captures.mkdirs();
            try (
                FileOutputStream out = new FileOutputStream(new File(captures, "android-ai-assistant.png"))) {
                screenshot.compress(Bitmap.CompressFormat.PNG, 100, out);
            }
        }
    }
    @Test
    public void httpsUsesSystemRootsAndRejectsExpiredCertificate() throws Exception {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        checkHttps(context, "https://open.bigmodel.cn/api/coding/paas/v4", false);
        checkHttps(context, "https://expired.badssl.com", true);
    }
    private void checkHttps(Context context, String endpoint, boolean expired) throws Exception {
        AIAssistantController controller = new AIAssistantController(context, endpoint);
        try {
            long deadline = System.currentTimeMillis() + 12000;
            while (!controller.state.ready && System.currentTimeMillis() < deadline) Thread.sleep(50);
            assertTrue(controller.state.error, controller.state.ready);
            controller.setListener(s -> {});
            CountDownLatch submitted = new CountDownLatch(1);
            AtomicBoolean sent = new AtomicBoolean();
            controller.send("TLS integration test", ok -> {
                sent.set(ok);
                submitted.countDown();
            });
            assertTrue(submitted.await(5, TimeUnit.SECONDS));
            assertTrue(sent.get());
            deadline = System.currentTimeMillis() + 30000;
            while (controller.state.error.isEmpty() && System.currentTimeMillis() < deadline)
                Thread.sleep(100);
            String error = controller.state.error;
            if (expired)
                assertTrue(error, error.contains("certificate") || error.contains("SSL"));
            else
                assertTrue(error, error.startsWith("HTTP "));
        } finally {
            controller.close();
        }
    }
}
