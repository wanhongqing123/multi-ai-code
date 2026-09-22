package com.kongshang.maichat;

import android.content.Context;
import android.graphics.Bitmap;
import android.view.View;
import android.view.inputmethod.InputMethodManager;
import android.widget.EditText;
import androidx.test.core.app.ActivityScenario;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import org.junit.Test;
import org.junit.runner.RunWith;
import java.io.File;
import java.io.FileOutputStream;
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
        Method method = object.getClass().getDeclaredMethod(name, types); method.setAccessible(true); method.invoke(object, args);
    }
    private interface Check { boolean check(MainActivity activity) throws Exception; }
    private void await(ActivityScenario<MainActivity> scenario, Check check) throws Exception {
        long deadline = System.currentTimeMillis() + 8000;
        AtomicBoolean done = new AtomicBoolean();
        while (!done.get() && System.currentTimeMillis() < deadline) {
            scenario.onActivity(activity -> { try { done.set(check.check(activity)); } catch (Exception error) { throw new AssertionError(error); } });
            Thread.sleep(30);
        }
        assertTrue("UI condition timed out", done.get());
    }
    @Test public void historyAndTypingSurviveIncomingMessagesAndKeyboardLocatesLatest() throws Exception {
        try (ActivityScenario<MainActivity> scenario = ActivityScenario.launch(MainActivity.class)) {
            AtomicReference<EditText> input = new AtomicReference<>();
            AtomicReference<String> anchor = new AtomicReference<>();
            scenario.onActivity(activity -> {
                try {
                    ((RemoteIMSessionController) get(activity, "session")).destroy();
                    File directory = new File(activity.getCacheDir(), "ui-fixture-" + System.nanoTime());
                    directory.mkdirs();
                    RemoteIMSessionController session = new RemoteIMSessionController(
                        new LocalSettingsStore(new File(directory, "settings")), new LocalChatHistoryStore(new File(directory, "history")));
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
                ((InputMethodManager) activity.getSystemService(Context.INPUT_METHOD_SERVICE)).showSoftInput(input.get(), InputMethodManager.SHOW_IMPLICIT);
            });
            await(scenario, activity -> {
                ChatMessageList list = (ChatMessageList) get(activity, "currentMessageList");
                androidx.core.view.WindowInsetsCompat insets = androidx.core.view.ViewCompat.getRootWindowInsets(activity.getWindow().getDecorView());
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
                return list.getCount() == 22 && list.getLastVisiblePosition() == 21 && input.get().getText().length() == 0;
            });
            Bitmap screenshot = InstrumentationRegistry.getInstrumentation().getUiAutomation().takeScreenshot();
            if (screenshot != null) {
                File dir = new File(InstrumentationRegistry.getInstrumentation().getTargetContext().getExternalFilesDir(null), "test-captures");
                dir.mkdirs();
                try (FileOutputStream output = new FileOutputStream(new File(dir, "chat-keyboard.png"))) { screenshot.compress(Bitmap.CompressFormat.PNG, 100, output); }
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
            await(scenario, activity -> ((ChatMessageList) get(activity, "currentMessageList")).getLastVisiblePosition() < 20);
            AtomicReference<String> beforeBack = new AtomicReference<>();
            scenario.onActivity(activity -> {
                try {
                    ChatMessageList list = (ChatMessageList) get(activity, "currentMessageList");
                    beforeBack.set(list.firstVisibleMessageId());
                    long now = android.os.SystemClock.uptimeMillis();
                    for (int i = 0; i < 3; i++) {
                        int action = i == 0 ? android.view.MotionEvent.ACTION_DOWN : i == 1 ? android.view.MotionEvent.ACTION_MOVE : android.view.MotionEvent.ACTION_CANCEL;
                        android.view.MotionEvent event = android.view.MotionEvent.obtain(now, now + i * 100, action, i == 0 ? 250 : 450, 400, 0);
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
