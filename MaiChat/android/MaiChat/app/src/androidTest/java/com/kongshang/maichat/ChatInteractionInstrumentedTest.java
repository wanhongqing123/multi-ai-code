// 文件作用：Android 设备集成测试；ChatInteractionInstrumentedTest 负责覆盖对应界面或服务的设备行为，本文件给出其验证。
// 运行边界：仅供自动化验证使用，不作为正式 App 或 Agent 的用户功能入口。
// 覆盖场景：historyAndTypingSurviveIncomingMessagesAndKeyboardLocatesLatest。
package com.kongshang.maichat;

import android.content.Context;
import android.app.UiAutomation;
import android.app.Dialog;
import android.content.res.Configuration;
import android.graphics.Bitmap;
import android.graphics.Color;
import android.os.SystemClock;
import android.view.View;
import android.view.ViewGroup;
import android.view.inputmethod.InputMethodManager;
import android.widget.EditText;
import android.widget.ImageView;
import android.widget.TextView;
import androidx.test.core.app.ActivityScenario;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import org.junit.Test;
import org.junit.runner.RunWith;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;
import static org.junit.Assert.*;

@RunWith(AndroidJUnit4.class)
public class ChatInteractionInstrumentedTest {
    private static Object get(Object object, String field) throws Exception {
        Field value = object.getClass().getDeclaredField(field); value.setAccessible(true); return value.get(object);
    }
    private static void set(Object object, String field, Object value) throws Exception {
        Field target = object.getClass().getDeclaredField(field); target.setAccessible(true); target.set(object, value);
    }
    private static void call(Object object, String name, Class<?>[] types, Object... args) throws Exception {
        Method method = object.getClass().getDeclaredMethod(name, types);
        method.setAccessible(true);
        method.invoke(object, args);
    }
    private interface Check { boolean check(MainActivity activity) throws Exception; }
    private static View findDescription(View root, String description) {
        if (root.getContentDescription() != null &&
            description.contentEquals(root.getContentDescription())) return root;
        if (root instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) root;
            for (int i = 0; i < group.getChildCount(); i++) {
                View found = findDescription(group.getChildAt(i), description);
                if (found != null) return found;
            }
        }
        return null;
    }
    private static boolean containsText(View root, String text) {
        if (root instanceof TextView && ((TextView) root).getText().toString().contains(text))
            return true;
        if (root instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) root;
            for (int i = 0; i < group.getChildCount(); i++)
                if (containsText(group.getChildAt(i), text)) return true;
        }
        return false;
    }
    private void await(ActivityScenario<MainActivity> scenario, Check check) throws Exception {
        long deadline = System.currentTimeMillis() + 8000;
        AtomicBoolean done = new AtomicBoolean();
        while (!done.get() && System.currentTimeMillis() < deadline) {
            scenario.onActivity(activity -> {
                try {
                    done.set(check.check(activity));
                } catch (Exception error) {
                    throw new AssertionError(error);
                }
            });
            Thread.sleep(30);
        }
        assertTrue("UI condition timed out", done.get());
    }
    @Test public void imImageBubbleOpensExpandedPreviewAndReturnsToChat() throws Exception {
        try (ActivityScenario<MainActivity> scenario = ActivityScenario.launch(MainActivity.class)) {
            AtomicReference<File> source = new AtomicReference<>();
            scenario.onActivity(activity -> {
                try {
                    ((RemoteIMSessionController) get(activity, "session")).destroy();
                    File directory = new File(activity.getCacheDir(), "image-preview-" + System.nanoTime());
                    directory.mkdirs();
                    RemoteIMSessionController session = new RemoteIMSessionController(
                        new LocalSettingsStore(new File(directory, "settings")),
                        new LocalChatHistoryStore(new File(directory, "history")));
                    session.login("image-owner");
                    session.addContact("image-peer");
                    File file = new File(directory, "photo.png");
                    Bitmap bitmap = Bitmap.createBitmap(320, 500, Bitmap.Config.ARGB_8888);
                    bitmap.eraseColor(Color.RED);
                    try (FileOutputStream output = new FileOutputStream(file)) {
                        assertTrue(bitmap.compress(Bitmap.CompressFormat.PNG, 100, output));
                    } finally { bitmap.recycle(); }
                    source.set(file);
                    session.chatState().queueOutgoingImageTo("image-peer", file.getAbsolutePath(),
                        320, 500, file.length());
                    set(activity, "session", session);
                    set(activity, "showInitialLogin", false);
                    call(activity, "render", new Class<?>[0]);
                    call(activity, "openChat", new Class<?>[]{String.class}, "image-peer");
                } catch (Exception error) { throw new AssertionError(error); }
            });
            await(scenario, activity -> {
                View sourceView = findDescription(activity.getWindow().getDecorView(), "预览图片");
                return sourceView instanceof ImageView && sourceView.getWidth() > 0 &&
                    ((ImageView) sourceView).getDrawable() != null;
            });
            SystemClock.sleep(500);
            scenario.onActivity(activity -> {
                View sourceView = findDescription(activity.getWindow().getDecorView(), "预览图片");
                assertNotNull(sourceView);
                assertTrue(sourceView.performClick());
            });
            await(scenario, activity -> {
                Dialog preview = (Dialog) get(activity, "imageDialog");
                return preview != null && preview.isShowing() &&
                    get(activity, "galleryImageView") != null;
            });
            long deadline = SystemClock.uptimeMillis() + 10000;
            boolean red = false;
            while (SystemClock.uptimeMillis() < deadline) {
                Bitmap screen = InstrumentationRegistry.getInstrumentation()
                    .getUiAutomation().takeScreenshot();
                if (screen != null) {
                    int color = screen.getPixel(screen.getWidth() / 2, screen.getHeight() / 2);
                    red = Color.red(color) > 180 && Color.blue(color) < 80;
                    screen.recycle();
                    if (red) break;
                }
                SystemClock.sleep(50);
            }
            assertTrue("Expanded Graphics image did not appear", red);
            SystemClock.sleep(600);
            scenario.onActivity(activity -> {
                try {
                    Dialog preview = (Dialog) get(activity, "imageDialog");
                    View close = findDescription(preview.getWindow().getDecorView(), "关闭图片预览");
                    assertNotNull(close);
                    assertTrue(close.performClick());
                } catch (Exception error) { throw new AssertionError(error); }
            });
            await(scenario, activity -> get(activity, "imageDialog") == null &&
                "image-peer".equals(get(activity, "activeChatUserId")));
            SystemClock.sleep(500);
            if (source.get() != null) source.get().delete();
        }
    }
    @Test public void landscapeShowsConversationBesideChatAndKeepsDraftOnRotation() throws Exception {
        try (ActivityScenario<MainActivity> scenario = ActivityScenario.launch(MainActivity.class)) {
            UiAutomation automation = InstrumentationRegistry.getInstrumentation().getUiAutomation();
            try {
                scenario.onActivity(activity -> {
                    try {
                        ((RemoteIMSessionController) get(activity, "session")).destroy();
                        File directory = new File(activity.getCacheDir(), "landscape-fixture-" + System.nanoTime());
                        directory.mkdirs();
                        RemoteIMSessionController session = new RemoteIMSessionController(
                            new LocalSettingsStore(new File(directory, "settings")),
                            new LocalChatHistoryStore(new File(directory, "history")));
                        session.login("landscape-owner");
                        session.addContact("landscape-peer");
                        session.addContact("landscape-other");
                        session.chatState().receiveText("横屏消息", "landscape-peer");
                        set(activity, "session", session);
                        set(activity, "showInitialLogin", false);
                        call(activity, "render", new Class<?>[0]);
                        call(activity, "openChat", new Class<?>[]{String.class}, "landscape-peer");
                        ((EditText) get(activity, "messageInput")).setText("横屏草稿");
                    } catch (Exception error) { throw new AssertionError(error); }
                });
                automation.setRotation(UiAutomation.ROTATION_FREEZE_90);
                await(scenario, activity -> {
                    if (activity.getResources().getConfiguration().orientation
                            != Configuration.ORIENTATION_LANDSCAPE ||
                        !(boolean) get(activity, "renderedLandscapeIm")) return false;
                    View sidebar = (View) get(activity, "landscapeConversationPane");
                    View chat = (View) get(activity, "currentMessageList");
                    EditText input = (EditText) get(activity, "messageInput");
                    if (sidebar == null || chat == null || sidebar.getWidth() == 0 ||
                        !"横屏草稿".equals(input.getText().toString())) return false;
                    int[] left = new int[2], right = new int[2];
                    sidebar.getLocationOnScreen(left);
                    chat.getLocationOnScreen(right);
                    return sidebar.isShown() && right[0] >= left[0] + sidebar.getWidth();
                });
                InstrumentationRegistry.getInstrumentation().waitForIdleSync();
                SystemClock.sleep(500);
                Bitmap screenshot = automation.takeScreenshot();
                assertNotNull(screenshot);
                File target = new File(InstrumentationRegistry.getInstrumentation()
                    .getTargetContext().getExternalFilesDir(null), "android-im-landscape.png");
                try (FileOutputStream output = new FileOutputStream(target)) {
                    assertTrue(screenshot.compress(Bitmap.CompressFormat.PNG, 100, output));
                } finally { screenshot.recycle(); }
                scenario.onActivity(activity -> {
                    try {
                        ((RemoteIMSessionController) get(activity, "session"))
                            .chatState().receiveText("横屏新消息", "landscape-peer");
                        activity.onStateChanged();
                    } catch (Exception error) { throw new AssertionError(error); }
                });
                await(scenario, activity -> containsText(
                    (View) get(activity, "currentConversationRows"), "横屏新消息"));
                AtomicReference<ChatMessageList> retainedChat = new AtomicReference<>();
                scenario.onActivity(activity -> {
                    try {
                        retainedChat.set((ChatMessageList) get(activity, "currentMessageList"));
                        File sample = new File(activity.getCacheDir(), "landscape-dock.mp4");
                        try (InputStream input = InstrumentationRegistry.getInstrumentation()
                                .getContext().getAssets().open("gallery-sample.mp4");
                             FileOutputStream output = new FileOutputStream(sample)) {
                            byte[] buffer = new byte[8192];
                            int count;
                            while ((count = input.read(buffer)) >= 0) output.write(buffer, 0, count);
                        }
                        assertTrue(activity.openAgentVideo(sample.getAbsolutePath()));
                    } catch (Exception error) { throw new AssertionError(error); }
                });
                await(scenario, activity -> {
                    View sidebar = (View) get(activity, "landscapeConversationPane");
                    MaiFfplayVideoView video = MaiFfplayVideoView.activeView();
                    return sidebar.getVisibility() == View.GONE && video != null &&
                        video.hasPresentedFrame() && retainedChat.get() == get(activity, "currentMessageList") &&
                        "横屏草稿".equals(((EditText) get(activity, "messageInput")).getText().toString());
                });
                scenario.onActivity(activity -> {
                    try {
                        set(activity, "holdingVoice", true);
                        set(activity, "speechText", "测试实时语音转写");
                        call(activity, "updateSpeechPreview", new Class<?>[0]);
                    } catch (Exception error) { throw new AssertionError(error); }
                });
                await(scenario, activity -> {
                    TextView overlay = (TextView) get(activity, "videoTranscriptOverlay");
                    TextView inline = (TextView) get(activity, "speechPreview");
                    return overlay != null && overlay.isShown() &&
                        inline != null && inline.getVisibility() == View.GONE &&
                        overlay.getText().toString().contains("测试实时语音转写");
                });
                InstrumentationRegistry.getInstrumentation().waitForIdleSync();
                SystemClock.sleep(200);
                Bitmap dockScreenshot = automation.takeScreenshot();
                assertNotNull(dockScreenshot);
                File dockTarget = new File(InstrumentationRegistry.getInstrumentation()
                    .getTargetContext().getExternalFilesDir(null), "android-im-landscape-video-dock.png");
                try (FileOutputStream output = new FileOutputStream(dockTarget)) {
                    assertTrue(dockScreenshot.compress(Bitmap.CompressFormat.PNG, 100, output));
                } finally { dockScreenshot.recycle(); }
                scenario.onActivity(activity -> {
                    try {
                        set(activity, "holdingVoice", false);
                        call(activity, "updateSpeechPreview", new Class<?>[0]);
                    } catch (Exception error) { throw new AssertionError(error); }
                });
                scenario.onActivity(activity ->
                    activity.setAssistantVoiceTranscript(true, "创作中心语音转写"));
                await(scenario, activity -> {
                    TextView overlay = (TextView) get(activity, "videoTranscriptOverlay");
                    return overlay != null && overlay.isShown() &&
                        overlay.getText().toString().contains("创作中心语音转写");
                });
                scenario.onActivity(activity -> activity.setAssistantVoiceTranscript(false, ""));
                scenario.onActivity(activity -> assertTrue(activity.commandAgentVideo("close", 0, 0, 0)));
                await(scenario, activity -> ((View) get(activity, "landscapeConversationPane"))
                    .getVisibility() == View.VISIBLE && retainedChat.get() == get(activity, "currentMessageList"));
                scenario.onActivity(activity -> {
                    try { call(activity, "openChat", new Class<?>[]{String.class}, "landscape-other"); }
                    catch (Exception error) { throw new AssertionError(error); }
                });
                await(scenario, activity -> "landscape-other".equals(get(activity, "activeChatUserId")) &&
                    ((EditText) get(activity, "messageInput")).getText().length() == 0 &&
                    ((View) get(activity, "landscapeConversationPane")).isShown());
                scenario.onActivity(activity -> {
                    try { call(activity, "openChat", new Class<?>[]{String.class}, "landscape-peer"); }
                    catch (Exception error) { throw new AssertionError(error); }
                });
                await(scenario, activity -> "landscape-peer".equals(get(activity, "activeChatUserId")) &&
                    "横屏草稿".equals(((EditText) get(activity, "messageInput")).getText().toString()));
                scenario.onActivity(activity -> {
                    View back = findDescription(activity.getWindow().getDecorView(), "返回会话列表");
                    assertNotNull(back);
                    assertTrue(back.performClick());
                });
                await(scenario, activity -> get(activity, "activeChatUserId") == null &&
                    (boolean) get(activity, "renderedLandscapeIm"));
                scenario.onActivity(activity -> {
                    try { call(activity, "openChat", new Class<?>[]{String.class}, "landscape-peer"); }
                    catch (Exception error) { throw new AssertionError(error); }
                });
                await(scenario, activity -> "landscape-peer".equals(get(activity, "activeChatUserId")) &&
                    "横屏草稿".equals(((EditText) get(activity, "messageInput")).getText().toString()));
                automation.setRotation(UiAutomation.ROTATION_FREEZE_0);
                await(scenario, activity ->
                    !(boolean) get(activity, "renderedLandscapeIm") &&
                    "landscape-peer".equals(get(activity, "activeChatUserId")) &&
                    "横屏草稿".equals(((EditText) get(activity, "messageInput")).getText().toString()));
            } finally {
                automation.setRotation(UiAutomation.ROTATION_FREEZE_0);
            }
        }
    }
    // 作用：验证接收新消息和打开键盘时草稿与最新消息位置不丢失。
    // 按真实调用顺序检查输入和可观察结果，失败时保留定位所需的上下文。
    @Test public void historyAndTypingSurviveIncomingMessagesAndKeyboardLocatesLatest() throws Exception {
        try (ActivityScenario<MainActivity> scenario = ActivityScenario.launch(MainActivity.class)) {
            AtomicReference<EditText> input = new AtomicReference<>();
            AtomicReference<String> anchor = new AtomicReference<>();
            scenario.onActivity(activity -> {
                try {
                    ((RemoteIMSessionController) get(activity, "session")).destroy();
                    File directory = new File(activity.getCacheDir(), "ui-fixture-" + System.nanoTime());
                    directory.mkdirs();
                    RemoteIMSessionController session =
                            new RemoteIMSessionController(new LocalSettingsStore(new File(directory, "settings")),
                                    new LocalChatHistoryStore(new File(directory, "history")));
                    session.login("ui-test-owner"); session.addContact("ui-test-peer");
                    for (int index = 0; index < 60; index++) session.chatState().receiveText(
                        "消息 " + index + "\n\n- **列表**与 `code`\n- 多行内容用于检查滚动位置。", "ui-test-peer");
                    set(activity, "session", session); set(activity, "showInitialLogin", false);
                    call(activity, "render", new Class<?>[0]);
                    call(activity, "openChat", new Class<?>[]{String.class}, "ui-test-peer");
                } catch (Exception error) { throw new AssertionError(error); }
            });
            await(scenario, activity -> {
                ChatMessageList list = (ChatMessageList) get(activity, "currentMessageList");
                return list.getCount() == 20 && list.getLastVisiblePosition() == 19;
            });
            scenario.onActivity(activity -> {
                try {
                    input.set((EditText) get(activity, "messageInput")); input.get().setText("草稿保留");
                    ChatMessageList list = (ChatMessageList) get(activity, "currentMessageList");
                    list.setSelectionFromTop(3, 0);
                } catch (Exception error) { throw new AssertionError(error); }
            });
            InstrumentationRegistry.getInstrumentation().waitForIdleSync();
            scenario.onActivity(activity -> {
                try {
                    ChatMessageList list = (ChatMessageList) get(activity, "currentMessageList");
                    anchor.set(list.firstVisibleMessageId());
                    RemoteIMSessionController session = (RemoteIMSessionController) get(activity, "session");
                    session.chatState().receiveText("新的来信不会夺走历史位置", "ui-test-peer");
                    activity.onStateChanged();
                } catch (Exception error) { throw new AssertionError(error); }
            });
            await(scenario, activity -> {
                ChatMessageList list = (ChatMessageList) get(activity, "currentMessageList");
                assertSame(input.get(), get(activity, "messageInput"));
                assertEquals("草稿保留", input.get().getText().toString());
                return list.getCount() == 21 && anchor.get().equals(list.firstVisibleMessageId());
            });
            scenario.onActivity(activity -> {
                input.get().requestFocus();
                ((InputMethodManager) activity.getSystemService(Context.INPUT_METHOD_SERVICE))
                        .showSoftInput(input.get(), InputMethodManager.SHOW_IMPLICIT);
            });
            await(scenario, activity -> {
                ChatMessageList list = (ChatMessageList) get(activity, "currentMessageList");
                androidx.core.view.WindowInsetsCompat insets =
                        androidx.core.view.ViewCompat.getRootWindowInsets(activity.getWindow().getDecorView());
                View host = (View) get(activity, "insetsHost");
                return (boolean) get(activity, "keyboardVisible") && !(boolean) get(activity, "imeAnimating")
                    && host.getPaddingBottom() > 150 * activity.getResources().getDisplayMetrics().density
                    && list.getLastVisiblePosition() == list.getCount() - 1;
            });
            scenario.onActivity(activity -> {
                try {
                    assertSame(input.get(), get(activity, "messageInput"));
                    input.get().setText("本地发送定位测试");
                    ((View) get(activity, "sendButton")).performClick();
                } catch (Exception error) { throw new AssertionError(error); }
            });
            await(scenario, activity -> {
                ChatMessageList list = (ChatMessageList) get(activity, "currentMessageList");
                return list.getCount() == 22 && list.getLastVisiblePosition() == 21
                        && input.get().getText().length() == 0;
            });
            Bitmap screenshot = InstrumentationRegistry.getInstrumentation().getUiAutomation().takeScreenshot();
            if (screenshot != null) {
                File dir = new File(
                        InstrumentationRegistry.getInstrumentation().getTargetContext().getExternalFilesDir(null),
                        "test-captures");
                dir.mkdirs();
                try (FileOutputStream output = new FileOutputStream(new File(dir, "chat-keyboard.png"))) {
                    screenshot.compress(Bitmap.CompressFormat.PNG, 100, output);
                }
                screenshot.recycle();
            }
            AtomicReference<String> historyTarget = new AtomicReference<>();
            scenario.onActivity(activity -> {
                try {
                    ChatMessageList list = (ChatMessageList) get(activity, "currentMessageList");
                    historyTarget.set(((RemoteIMMessage) list.getAdapter().getItem(8)).id());
                    list.showMessage(historyTarget.get());
                } catch (Exception error) { throw new AssertionError(error); }
            });
            await(scenario,
                    activity -> ((ChatMessageList) get(activity, "currentMessageList")).getLastVisiblePosition() < 20);
            AtomicReference<String> beforeBack = new AtomicReference<>();
            scenario.onActivity(activity -> {
                try {
                    ChatMessageList list = (ChatMessageList) get(activity, "currentMessageList");
                    beforeBack.set(list.firstVisibleMessageId());
                    long now = android.os.SystemClock.uptimeMillis();
                    for (int i = 0; i < 3; i++) {
                        int action = i == 0 ? android.view.MotionEvent.ACTION_DOWN
                                : i == 1    ? android.view.MotionEvent.ACTION_MOVE
                                            : android.view.MotionEvent.ACTION_CANCEL;
                        android.view.MotionEvent event =
                                android.view.MotionEvent.obtain(now, now + i * 100, action, i == 0 ? 250 : 450, 400, 0);
                        activity.dispatchTouchEvent(event); event.recycle();
                    }
                } catch (Exception error) { throw new AssertionError(error); }
            });
            await(scenario, activity -> {
                ChatSwipeBack back = (ChatSwipeBack) get(activity, "swipeBack");
                ChatMessageList list = (ChatMessageList) get(activity, "currentMessageList");
                return !back.active() && beforeBack.get().equals(list.firstVisibleMessageId());
            });
            scenario.onActivity(activity -> {
                try { ((ChatSwipeBack) get(activity, "swipeBack")).goBack(); }
                catch (Exception error) { throw new AssertionError(error); }
            });
            await(scenario, activity -> get(activity, "activeChatUserId") == null);
        }
    }
}
