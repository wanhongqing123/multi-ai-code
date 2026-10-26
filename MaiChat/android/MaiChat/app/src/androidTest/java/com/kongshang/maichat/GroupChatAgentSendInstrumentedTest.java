package com.kongshang.maichat;

import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;
import static org.junit.Assume.assumeTrue;

import android.app.Instrumentation;
import android.content.Context;
import android.content.Intent;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import com.kongshang.maichat.tools.MaiChatHostTools;
import java.io.File;
import java.io.FileOutputStream;
import java.lang.reflect.Field;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.FutureTask;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import org.json.JSONObject;
import org.junit.Test;
import org.junit.runner.RunWith;

/** Sends exactly one opt-in disposable group message through the Agent host dispatch. */
@RunWith(AndroidJUnit4.class)
public final class GroupChatAgentSendInstrumentedTest {
    @Test public void hostDispatchWaitsForSdkSendReceipt() throws Exception {
        String groupId = InstrumentationRegistry.getArguments().getString("group_id","");
        String text = InstrumentationRegistry.getArguments().getString("text","");
        assumeTrue(BuildConfig.DEBUG && BuildConfig.APPLICATION_ID.endsWith(".isolated")
            && !BuildConfig.USE_LOCAL_UI_AUTH && !groupId.isEmpty() && !text.isEmpty());
        Instrumentation instrumentation = InstrumentationRegistry.getInstrumentation();
        Context context = instrumentation.getTargetContext();
        Intent intent = new Intent(context,MainActivity.class);
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        MainActivity activity = (MainActivity) instrumentation.startActivitySync(intent);
        try {
            RemoteIMSessionController session = (RemoteIMSessionController) field(activity,"session");
            MaiChatHostTools host = (MaiChatHostTools) field(activity,"maiChatHostTools");
            long deadline = System.currentTimeMillis() + 25_000;
            boolean ready = false;
            while (System.currentTimeMillis() < deadline) {
                AtomicReference<Boolean> state = new AtomicReference<>(false);
                instrumentation.runOnMainSync(() -> state.set(
                    session.groupTransportReady()
                        && session.group(groupId) != null));
                if (state.get()) { ready = true; break; }
                Thread.sleep(120);
            }
            assertTrue("Group unavailable; no send attempted",ready);
            FutureTask<JSONObject> send = new FutureTask<>(() -> (JSONObject)host.execute(
                "maichat_send_group_text",new JSONObject().put("group_id",groupId)
                    .put("text",text)));
            new Thread(send,"group-agent-send-test").start();
            JSONObject receipt = send.get(30,TimeUnit.SECONDS);
            assertTrue(receipt.getBoolean("queued"));
            assertTrue(receipt.getBoolean("sent"));
            assertNotNull(receipt.getString("remote_id"));
            assertTrue(!receipt.getString("remote_id").isEmpty());
            File directory = new File(context.getExternalFilesDir(null),"group-chat-e2e");
            assertTrue(directory.mkdirs() || directory.isDirectory());
            try (FileOutputStream output = new FileOutputStream(
                new File(directory,"agent-send-receipt.json"))) {
                output.write(receipt.toString().getBytes(StandardCharsets.UTF_8));
            }
        } finally {
            instrumentation.runOnMainSync(activity::finish);
        }
    }

    private static Object field(MainActivity activity,String name) throws Exception {
        Field field = MainActivity.class.getDeclaredField(name);
        field.setAccessible(true);
        return field.get(activity);
    }
}
