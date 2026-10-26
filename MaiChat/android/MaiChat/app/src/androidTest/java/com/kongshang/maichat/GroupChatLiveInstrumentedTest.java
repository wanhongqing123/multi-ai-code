package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;
import static org.junit.Assume.assumeTrue;

import android.app.Instrumentation;
import android.content.Context;
import android.content.Intent;
import android.graphics.Bitmap;
import android.os.Bundle;
import android.view.View;
import android.view.ViewGroup;
import android.widget.EditText;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import java.io.File;
import java.io.FileOutputStream;
import java.lang.reflect.Field;
import java.util.List;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.Test;
import org.junit.runner.RunWith;

/** Opt-in real Tencent group UI exchange on a disposable, already signed-in account. */
@RunWith(AndroidJUnit4.class)
public final class GroupChatLiveInstrumentedTest {
    @Test public void receivesIosGroupAndSendsAndroidReply() throws Exception {
        Bundle arguments = InstrumentationRegistry.getArguments();
        String groupId = arguments.getString("group_id","");
        String groupName = arguments.getString("group_name","");
        String incomingText = arguments.getString("incoming_text","");
        String thirdText = arguments.getString("third_text","");
        String replyText = arguments.getString("reply_text","");
        assumeTrue(BuildConfig.DEBUG && BuildConfig.APPLICATION_ID.endsWith(".isolated")
            && !BuildConfig.USE_LOCAL_UI_AUTH && !groupId.isEmpty() && !groupName.isEmpty()
            && !incomingText.isEmpty() && !replyText.isEmpty());

        Instrumentation instrumentation = InstrumentationRegistry.getInstrumentation();
        Context context = instrumentation.getTargetContext();
        Intent intent = new Intent(context,MainActivity.class);
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        MainActivity activity = (MainActivity) instrumentation.startActivitySync(intent);
        try {
            RemoteIMSessionController session = session(activity);
            assertTrue("IM failed to connect",waitUntil(instrumentation,15_000,() ->
                session.connectionState() == TencentIMClient.ConnectionState.CONNECTED));
            assertTrue("group missing on Android",waitUntil(instrumentation,25_000,() ->
                session.group(groupId) != null));
            AtomicReference<RemoteIMGroup> group = new AtomicReference<>();
            instrumentation.runOnMainSync(() -> group.set(session.group(groupId)));
            assertEquals(groupName,group.get().name());
            assertEquals(3,group.get().memberCount());
            assertTrue("iOS group message missing",waitUntil(instrumentation,25_000,() ->
                countText(session.groupMessages(groupId),incomingText) == 1));
            assertEquals(1,countText(session.groupMessages(groupId),incomingText));
            if (!thirdText.isEmpty()) {
                assertTrue("Desktop group message missing",waitUntil(instrumentation,25_000,() ->
                    countText(session.groupMessages(groupId),thirdText) == 1));
                assertEquals(1,countText(session.groupMessages(groupId),thirdText));
            }

            AtomicReference<View> row = new AtomicReference<>();
            instrumentation.runOnMainSync(() -> row.set(findDescription(
                activity.getWindow().getDecorView(),groupName + " 群聊")));
            assertNotNull("group list row missing",row.get());
            instrumentation.runOnMainSync(() -> row.get().performClick());
            assertNotNull("group composer missing",waitForComposer(instrumentation,activity,5_000));
            instrumentation.runOnMainSync(() -> {
                EditText input = findComposer(activity.getWindow().getDecorView());
                View send = findDescription(activity.getWindow().getDecorView(),"发送群消息");
                assertNotNull(input); assertNotNull(send);
                input.setText(replyText);
                send.performClick();
            });
            assertTrue("Android reply not acknowledged",waitUntil(instrumentation,20_000,() -> {
                for (RemoteIMGroupMessage message : session.groupMessages(groupId))
                    if (replyText.equals(message.text()) && !message.remoteId().isEmpty()
                        && message.status() == RemoteIMGroupMessage.Status.SENT) return true;
                return false;
            }));
            assertEquals(1,countText(session.groupMessages(groupId),replyText));
            capture(instrumentation,context,"B-after-send");
        } finally {
            instrumentation.runOnMainSync(activity::finish);
        }
    }

    private interface Check { boolean okay() throws Exception; }

    private static boolean waitUntil(Instrumentation instrumentation,long timeout,Check check)
            throws Exception {
        long deadline = System.currentTimeMillis() + timeout;
        while (System.currentTimeMillis() < deadline) {
            AtomicReference<Boolean> value = new AtomicReference<>(false);
            instrumentation.runOnMainSync(() -> {
                try { value.set(check.okay()); }
                catch (Exception ignored) { value.set(false); }
            });
            if (value.get()) return true;
            Thread.sleep(120);
        }
        return false;
    }

    private static EditText waitForComposer(Instrumentation instrumentation,MainActivity activity,
                                             long timeout) throws Exception {
        AtomicReference<EditText> found = new AtomicReference<>();
        assertTrue(waitUntil(instrumentation,timeout,() -> {
            found.set(findComposer(activity.getWindow().getDecorView()));
            return found.get() != null;
        }));
        return found.get();
    }

    private static RemoteIMSessionController session(MainActivity activity) throws Exception {
        Field field = MainActivity.class.getDeclaredField("session");
        field.setAccessible(true);
        return (RemoteIMSessionController) field.get(activity);
    }

    private static int countText(List<RemoteIMGroupMessage> messages,String text) {
        int count = 0;
        for (RemoteIMGroupMessage message : messages)
            if (text.equals(message.text())) count++;
        return count;
    }

    private static EditText findComposer(View view) {
        if (view instanceof EditText && ((EditText)view).getHint() != null
            && "发送文字消息".contentEquals(((EditText)view).getHint()))
            return (EditText)view;
        if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup)view;
            for (int index = 0; index < group.getChildCount(); index++) {
                EditText found = findComposer(group.getChildAt(index));
                if (found != null) return found;
            }
        }
        return null;
    }

    private static View findDescription(View view,String description) {
        if (view.getContentDescription() != null
            && description.contentEquals(view.getContentDescription())) return view;
        if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup)view;
            for (int index = 0; index < group.getChildCount(); index++) {
                View found = findDescription(group.getChildAt(index),description);
                if (found != null) return found;
            }
        }
        return null;
    }

    private static void capture(Instrumentation instrumentation,Context context,String name)
            throws Exception {
        instrumentation.waitForIdleSync();
        Bitmap image = instrumentation.getUiAutomation().takeScreenshot();
        assertNotNull(image);
        File directory = new File(context.getExternalFilesDir(null),"group-chat-e2e");
        assertTrue(directory.mkdirs() || directory.isDirectory());
        try (FileOutputStream output = new FileOutputStream(new File(directory,name + ".png"))) {
            assertTrue(image.compress(Bitmap.CompressFormat.PNG,100,output));
        }
        image.recycle();
    }
}
