// 文件作用：在 Android 真机上验证通讯录好友资料页的进入、返回和安全删除确认。
// 运行边界：仅测试包用本地假会话；不登录账号、不请求网络、不删除真实好友。
package com.kongshang.maichat;

import static org.junit.Assert.*;

import android.app.UiAutomation;
import android.graphics.Bitmap;
import android.os.SystemClock;
import android.view.View;
import android.view.ViewGroup;
import android.view.accessibility.AccessibilityNodeInfo;
import android.widget.TextView;

import androidx.test.core.app.ActivityScenario;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;

import org.junit.Test;
import org.junit.runner.RunWith;

import java.io.File;
import java.io.FileOutputStream;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.List;
import java.util.Map;
import java.util.concurrent.atomic.AtomicReference;

@RunWith(AndroidJUnit4.class)
public class FriendProfileInstrumentedTest {
    private static Object get(Object object, String field) throws Exception {
        Field value = object.getClass().getDeclaredField(field);
        value.setAccessible(true);
        return value.get(object);
    }

    private static void set(Object object, String field, Object value) throws Exception {
        Field target = object.getClass().getDeclaredField(field);
        target.setAccessible(true);
        target.set(object, value);
    }

    private static void render(MainActivity activity) throws Exception {
        Method method = MainActivity.class.getDeclaredMethod("render");
        method.setAccessible(true);
        method.invoke(activity);
    }

    private static View findText(View root, String text) {
        if (root instanceof TextView && text.contentEquals(((TextView) root).getText()))
            return root;
        if (root instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) root;
            for (int index = 0; index < group.getChildCount(); index++) {
                View found = findText(group.getChildAt(index), text);
                if (found != null) return found;
            }
        }
        return null;
    }

    private static View findDescription(View root, String description) {
        if (root.getContentDescription() != null &&
            description.contentEquals(root.getContentDescription())) return root;
        if (root instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) root;
            for (int index = 0; index < group.getChildCount(); index++) {
                View found = findDescription(group.getChildAt(index), description);
                if (found != null) return found;
            }
        }
        return null;
    }

    private static void clickAncestor(View view) {
        assertNotNull(view);
        View current = view;
        while (current != null && !current.isClickable()) {
            if (!(current.getParent() instanceof View)) break;
            current = (View) current.getParent();
        }
        assertNotNull(current);
        assertTrue(current.isClickable());
        assertTrue(current.performClick());
    }

    private static void screenshot(File target) throws Exception {
        Bitmap bitmap = InstrumentationRegistry.getInstrumentation()
            .getUiAutomation().takeScreenshot();
        assertNotNull(bitmap);
        try (FileOutputStream output = new FileOutputStream(target)) {
            assertTrue(bitmap.compress(Bitmap.CompressFormat.PNG, 100, output));
        } finally { bitmap.recycle(); }
    }

    private static AccessibilityNodeInfo awaitDialogText(String text) {
        UiAutomation automation = InstrumentationRegistry.getInstrumentation().getUiAutomation();
        long deadline = SystemClock.uptimeMillis() + 5000;
        while (SystemClock.uptimeMillis() < deadline) {
            AccessibilityNodeInfo root = automation.getRootInActiveWindow();
            if (root != null) {
                List<AccessibilityNodeInfo> matches = root.findAccessibilityNodeInfosByText(text);
                if (!matches.isEmpty()) return root;
                root.recycle();
            }
            SystemClock.sleep(80);
        }
        fail("Confirmation dialog did not appear: " + text);
        return null;
    }

    @Test public void friendProfileOpensBeforeChatAndDeleteCanBeCancelled() throws Exception {
        try (ActivityScenario<MainActivity> scenario = ActivityScenario.launch(MainActivity.class)) {
            AtomicReference<File> captureDir = new AtomicReference<>();
            scenario.onActivity(activity -> {
                try {
                    ((RemoteIMSessionController) get(activity, "session")).destroy();
                    File directory = new File(activity.getCacheDir(), "friend-profile-test");
                    assertTrue(directory.mkdirs() || directory.isDirectory());
                    RemoteIMSessionController session = new RemoteIMSessionController(
                        new LocalSettingsStore(new File(directory, "settings")),
                        new LocalChatHistoryStore(new File(directory, "history")));
                    session.login("mowner");
                    session.addContact("mfriend");
                    session.chatState().upsertContact(
                        new RemoteIMContact("mfriend", "好友测试"));
                    @SuppressWarnings("unchecked")
                    Map<String, String> accountIds =
                        (Map<String, String>) get(session, "friendAccountByIm");
                    accountIds.put("mfriend", "m_friend");
                    set(activity, "session", session);
                    set(activity, "showInitialLogin", false);
                    set(activity, "activeTab", RemoteIMTab.CONTACTS);
                    render(activity);
                    captureDir.set(activity.getExternalFilesDir(null));
                } catch (Exception error) { throw new AssertionError(error); }
            });

            scenario.onActivity(activity -> {
                View root = activity.getWindow().getDecorView();
                clickAncestor(findText(root, "好友测试"));
                assertNotNull(findText(root, "好友资料"));
                assertNotNull(findText(root, "MaiChat ID"));
                assertNotNull(findText(root, "m_friend"));
                assertNotNull(findText(root, "发消息"));
                assertNotNull(findText(root, "删除好友"));
            });
            SystemClock.sleep(350);
            screenshot(new File(captureDir.get(), "friend-profile-09cdf77c.png"));

            scenario.onActivity(activity -> {
                View root = activity.getWindow().getDecorView();
                clickAncestor(findDescription(root, "返回通讯录"));
                assertNull(getUnchecked(activity, "activeFriendProfileUserId"));
                clickAncestor(findText(root, "好友测试"));
                clickAncestor(findText(root, "发消息"));
                assertEquals("mfriend", getUnchecked(activity, "activeChatUserId"));
                assertNotNull(findText(root, "好友测试"));
            });

            scenario.onActivity(activity -> {
                try {
                    set(activity, "activeChatUserId", null);
                    set(activity, "activeTab", RemoteIMTab.CONTACTS);
                    render(activity);
                    clickAncestor(findText(activity.getWindow().getDecorView(), "好友测试"));
                    clickAncestor(findText(activity.getWindow().getDecorView(), "删除好友"));
                } catch (Exception error) { throw new AssertionError(error); }
            });
            AccessibilityNodeInfo dialog = awaitDialogText("删除好友？");
            SystemClock.sleep(250);
            screenshot(new File(captureDir.get(), "friend-delete-confirm-09cdf77c.png"));
            try {
                List<AccessibilityNodeInfo> cancel =
                    dialog.findAccessibilityNodeInfosByText("取消");
                assertFalse(cancel.isEmpty());
                assertTrue(cancel.get(0).performAction(AccessibilityNodeInfo.ACTION_CLICK));
            } finally { dialog.recycle(); }
            scenario.onActivity(activity -> {
                RemoteIMSessionController session =
                    (RemoteIMSessionController) getUnchecked(activity, "session");
                assertTrue(session.acceptedFriends().stream().anyMatch(
                    contact -> "mfriend".equals(contact.userId())));
            });
        }
    }

    private static Object getUnchecked(Object object, String field) {
        try { return get(object, field); }
        catch (Exception error) { throw new AssertionError(error); }
    }
}
