package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import android.content.Context;
import android.database.Cursor;
import android.database.sqlite.SQLiteDatabase;
import androidx.test.core.app.ApplicationProvider;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.List;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class GroupChatParityTest {
    @Test public void activationPayloadMatchesIosGoldenBytesAndRejectsOtherCustomMessages() {
        byte[] iosPayload = "{\"type\":\"maichat_group_activation\",\"v\":1}"
            .getBytes(StandardCharsets.UTF_8);
        assertEquals(41,iosPayload.length);
        assertTrue(Arrays.equals(iosPayload,TencentIMClient.GROUP_ACTIVATION));
        assertTrue(TencentIMClient.isGroupActivation(iosPayload));
        assertFalse(TencentIMClient.isGroupActivation(
            "{\"v\":1,\"type\":\"maichat_group_activation\"}"
                .getBytes(StandardCharsets.UTF_8)));
        assertFalse(TencentIMClient.isGroupActivation(new byte[]{1,2,3}));
    }

    @Test public void groupCacheSeparatesAccountsAndPagesWithoutTouchingDirectChats() {
        Context context = ApplicationProvider.getApplicationContext();
        String database = "maichat-groups-robolectric.db";
        context.deleteDatabase(database);
        AndroidChatHistoryStore store = new AndroidChatHistoryStore(context,database);
        RemoteIMGroup first = new RemoteIMGroup("group-a","测试群",3,true);
        store.upsertGroup("owner-a",first);
        store.upsertGroup("owner-b",new RemoteIMGroup("group-b","另一个账号",2));
        for (int index = 1; index <= 25; index++)
            store.upsertGroupMessage("owner-a",new RemoteIMGroupMessage(
                "local-" + index,"sdk-" + index,"group-a",
                index % 2 == 0 ? "owner-a" : "friend-a", "正文 " + index,
                1000L + index,RemoteIMGroupMessage.Status.RECEIVED));
        store.upsertGroupMessage("owner-b",new RemoteIMGroupMessage(
            "foreign","sdk-foreign","group-b","friend-b","私有",9000L,
            RemoteIMGroupMessage.Status.RECEIVED));

        List<RemoteIMGroup> groups = store.loadGroups("owner-a");
        assertEquals(1,groups.size());
        assertEquals("group-a",groups.get(0).groupId());
        assertTrue(groups.get(0).pendingActivation());
        List<RemoteIMGroupMessage> recent = store.loadGroupPage("owner-a","group-a",null,null,20);
        assertEquals(20,recent.size());
        assertEquals("正文 6",recent.get(0).text());
        assertEquals("正文 25",recent.get(19).text());
        List<RemoteIMGroupMessage> earlier = store.loadGroupPage("owner-a","group-a",
            recent.get(0).createdAtMillis(),recent.get(0).id(),20);
        assertEquals(5,earlier.size());
        assertEquals("正文 1",earlier.get(0).text());
        assertEquals(1,store.loadGroupSummaries("owner-a").size());
        assertEquals("正文 25",store.loadGroupSummaries("owner-a").get(0).text());
        assertTrue(store.loadGroupPage("owner-a","group-b",null,null,20).isEmpty());

        store.replaceGroups("owner-a",List.of(first.withPendingActivation(false)));
        assertFalse(store.loadGroups("owner-a").get(0).pendingActivation());
        store.deleteGroupMessage("owner-a","local-25");
        assertEquals("正文 24",store.loadGroupSummaries("owner-a").get(0).text());
        RemoteIMGroupMessage repeated = new RemoteIMGroupMessage(null,"sdk-24","group-a",
            "owner-a","正文 24",1024L,RemoteIMGroupMessage.Status.SENT);
        assertEquals(repeated.id(),new RemoteIMGroupMessage(null,"sdk-24","group-a",
            "owner-a","正文 24",1024L,RemoteIMGroupMessage.Status.SENT).id());
        store.upsertGroupMessage("owner-a",repeated);
        store.close();
        store = new AndroidChatHistoryStore(context,database);
        store.upsertGroupMessage("owner-a",new RemoteIMGroupMessage(null,"sdk-24","group-a",
            "owner-a","正文 24",1024L,RemoteIMGroupMessage.Status.SENT));
        try (Cursor cursor = store.getReadableDatabase().rawQuery(
            "SELECT COUNT(*) FROM group_messages WHERE owner_id=? AND group_id=? AND remote_id=?",
            new String[]{"owner-a","group-a","sdk-24"})) {
            assertTrue(cursor.moveToFirst());
            assertEquals(1,cursor.getInt(0));
        }
        store.close();
        context.deleteDatabase(database);
    }

    @Test public void upgradesVersionSevenByRemovingOnlyDuplicateRemoteMessages() {
        Context context = ApplicationProvider.getApplicationContext();
        String databaseName = "maichat-groups-v7-upgrade.db";
        context.deleteDatabase(databaseName);
        SQLiteDatabase old = context.openOrCreateDatabase(databaseName,Context.MODE_PRIVATE,null);
        old.execSQL("CREATE TABLE group_messages (owner_id TEXT NOT NULL,id TEXT NOT NULL,"
            + "remote_id TEXT NOT NULL DEFAULT '',group_id TEXT NOT NULL,sender_id TEXT NOT NULL,"
            + "body TEXT NOT NULL,status TEXT NOT NULL,created_at INTEGER NOT NULL,"
            + "PRIMARY KEY(owner_id,id))");
        old.execSQL("INSERT INTO group_messages VALUES"
            + "('owner','old','sdk-1','group','friend','older','RECEIVED',100),"
            + "('owner','new','sdk-1','group','friend','newer','RECEIVED',100),"
            + "('owner','pending','','group','owner','pending','PENDING',101)");
        old.setVersion(7);
        old.close();
        AndroidChatHistoryStore upgraded = new AndroidChatHistoryStore(context,databaseName);
        List<RemoteIMGroupMessage> messages = upgraded.loadGroupPage("owner","group",null,null,20);
        assertEquals(2,messages.size());
        assertEquals("newer",messages.get(0).text());
        assertEquals("pending",messages.get(1).text());
        upgraded.upsertGroupMessage("owner",new RemoteIMGroupMessage(
            "third","sdk-1","group","friend","latest",100,
            RemoteIMGroupMessage.Status.RECEIVED));
        assertEquals(2,upgraded.loadGroupPage("owner","group",null,null,20).size());
        upgraded.close();
        context.deleteDatabase(databaseName);
    }
}
