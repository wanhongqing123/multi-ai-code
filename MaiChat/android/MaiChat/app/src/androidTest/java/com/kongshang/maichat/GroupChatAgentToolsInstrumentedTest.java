package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;
import static org.junit.Assume.assumeTrue;

import android.app.Instrumentation;
import android.content.Context;
import android.content.Intent;
import android.os.Bundle;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import com.kongshang.maichat.tools.MaiChatHostTools;
import java.lang.reflect.Field;
import java.util.concurrent.FutureTask;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.Test;
import org.junit.runner.RunWith;

/** Read-only Agent calls against the same real joined group shown in the App. */
@RunWith(AndroidJUnit4.class)
public final class GroupChatAgentToolsInstrumentedTest {
    @Test public void listAndReadTheJoinedGroupThroughHostTools() throws Exception {
        String groupId = InstrumentationRegistry.getArguments().getString("group_id","");
        assumeTrue(BuildConfig.DEBUG && BuildConfig.APPLICATION_ID.endsWith(".isolated")
            && !BuildConfig.USE_LOCAL_UI_AUTH && !groupId.isEmpty());
        Instrumentation instrumentation = InstrumentationRegistry.getInstrumentation();
        Context context = instrumentation.getTargetContext();
        Intent intent = new Intent(context,MainActivity.class);
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        MainActivity activity = (MainActivity) instrumentation.startActivitySync(intent);
        try {
            RemoteIMSessionController session = (RemoteIMSessionController) field(activity,"session");
            MaiChatHostTools host = (MaiChatHostTools) field(activity,"maiChatHostTools");
            long deadline = System.currentTimeMillis() + 25_000;
            while (System.currentTimeMillis() < deadline) {
                AtomicReference<Boolean> ready = new AtomicReference<>(false);
                instrumentation.runOnMainSync(() -> ready.set(
                    session.groupTransportReady()
                        && session.group(groupId) != null));
                if (ready.get()) break;
                Thread.sleep(120);
            }
            AtomicReference<JSONObject> listed = new AtomicReference<>();
            instrumentation.runOnMainSync(() -> {
                try { listed.set((JSONObject)host.execute("maichat_list_groups",
                    new JSONObject().put("limit",20))); }
                catch (Exception error) { throw new AssertionError(error); }
            });
            assertNotNull(listed.get());
            JSONArray groups = listed.get().getJSONArray("groups");
            JSONObject selected = null;
            for (int index = 0; index < groups.length(); index++)
                if (groupId.equals(groups.getJSONObject(index).getString("group_id")))
                    selected = groups.getJSONObject(index);
            assertNotNull("Group missing from Agent list",selected);
            assertEquals(3,selected.getInt("member_count"));
            assertTrue(selected.has("unread_count"));

            FutureTask<JSONObject> read = new FutureTask<>(() -> (JSONObject)host.execute(
                "maichat_get_group_messages",new JSONObject().put("group_id",groupId)
                    .put("limit",20)));
            new Thread(read,"group-agent-read-test").start();
            JSONObject result = read.get(30,TimeUnit.SECONDS);
            assertEquals(groupId,result.getString("group_id"));
            JSONArray messages = result.getJSONArray("messages");
            assertTrue(messages.length() >= 4);
            for (int index = 0; index < messages.length(); index++) {
                JSONObject message = messages.getJSONObject(index);
                assertTrue(message.has("sender_id"));
                assertTrue(message.has("direction"));
                assertTrue(message.has("text"));
                assertTrue(message.has("remote_id"));
                assertTrue(message.has("created_at_ms"));
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
