package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import android.content.Context;
import android.database.sqlite.SQLiteDatabase;

import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;

import org.junit.Test;
import org.junit.runner.RunWith;

import java.util.List;

@RunWith(AndroidJUnit4.class)
public class AndroidChatHistoryStoreInstrumentedTest {
    private static final String TEST_DATABASE_NAME = "maichat-history-instrumented-test.db";

    @Test
    public void testIncrementalUpsertSummaryPaginationAndConversationDelete() {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        context.deleteDatabase(TEST_DATABASE_NAME);
        AndroidChatHistoryStore store = new AndroidChatHistoryStore(context, TEST_DATABASE_NAME);
        String owner = "android-test-owner";
        String peer = "mac-test-peer";
        store.upsertContact(owner, new RemoteIMContact(peer, "Mac Test"));

        for (int index = 0; index < 55; index += 1) {
            RemoteIMMessage message = new RemoteIMMessage(
                "local-" + index,
                "remote-" + index,
                index % 2 == 0 ? owner : peer,
                index % 2 == 0 ? peer : owner,
                "message-" + index,
                index % 2 == 0
                    ? RemoteIMMessage.Direction.OUTGOING
                    : RemoteIMMessage.Direction.INCOMING,
                index % 2 == 0
                    ? RemoteIMMessage.Status.SENT
                    : RemoteIMMessage.Status.RECEIVED,
                1_000L + index,
                null,
                null,
                null,
                null,
                RemoteIMOrigin.HUMAN
            );
            store.upsertMessage(owner, message);
        }

        assertEquals(1, store.loadContacts(owner).size());
        List<RemoteIMMessage> summaries = store.loadConversationSummaries(owner);
        assertEquals(1, summaries.size());
        assertEquals("message-54", summaries.get(0).text());

        AndroidChatHistoryStore.Page first = store.loadConversationPage(
            owner,
            peer,
            null,
            null,
            50
        );
        assertEquals(50, first.messages().size());
        assertTrue(first.hasEarlier());
        RemoteIMMessage oldest = first.messages().get(0);
        AndroidChatHistoryStore.Page second = store.loadConversationPage(
            owner,
            peer,
            oldest.createdAtMillis(),
            oldest.id(),
            50
        );
        assertEquals(5, second.messages().size());
        assertFalse(second.hasEarlier());
        assertTrue(store.containsRemoteId(owner, "remote-54"));

        store.deleteConversation(owner, peer);
        assertTrue(store.loadConversationPage(owner, peer, null, null, 50).messages().isEmpty());
        assertEquals(1, store.loadContacts(owner).size());
        store.close();
        context.deleteDatabase(TEST_DATABASE_NAME);
    }

    @Test
    public void keepsExistingHistoryWhenVideoColumnsAreAdded() {
        // 迁移前的 onUpgrade 是 DROP TABLE 再重建，升个版本就把用户聊天记录清空了。
        // 这条用例锁住"加列不能丢历史"。
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        String databaseName = "maichat-history-migration-test.db";
        context.deleteDatabase(databaseName);

        // 先按 v1 建库并写入一条消息。
        SQLiteDatabase legacy = context.openOrCreateDatabase(databaseName, Context.MODE_PRIVATE, null);
        legacy.execSQL(
            "CREATE TABLE contacts ("
                + "owner_id TEXT NOT NULL,"
                + "user_id TEXT NOT NULL,"
                + "display_name TEXT NOT NULL,"
                + "PRIMARY KEY(owner_id, user_id))"
        );
        legacy.execSQL(
            "CREATE TABLE messages ("
                + "owner_id TEXT NOT NULL,"
                + "id TEXT NOT NULL,"
                + "remote_id TEXT NOT NULL DEFAULT '',"
                + "peer_id TEXT NOT NULL,"
                + "from_id TEXT NOT NULL,"
                + "to_id TEXT NOT NULL,"
                + "body TEXT NOT NULL,"
                + "direction TEXT NOT NULL,"
                + "status TEXT NOT NULL,"
                + "created_at INTEGER NOT NULL,"
                + "image_path TEXT NOT NULL DEFAULT '',"
                + "image_width INTEGER NOT NULL DEFAULT 0,"
                + "image_height INTEGER NOT NULL DEFAULT 0,"
                + "image_size INTEGER NOT NULL DEFAULT 0,"
                + "voice_path TEXT NOT NULL DEFAULT '',"
                + "voice_duration INTEGER NOT NULL DEFAULT 0,"
                + "file_path TEXT NOT NULL DEFAULT '',"
                + "file_name TEXT NOT NULL DEFAULT '',"
                + "file_mime TEXT NOT NULL DEFAULT '',"
                + "file_size INTEGER NOT NULL DEFAULT 0,"
                + "origin TEXT NOT NULL DEFAULT 'human',"
                + "PRIMARY KEY(owner_id, id))"
        );
        legacy.execSQL(
            "INSERT INTO messages (owner_id,id,peer_id,from_id,to_id,body,direction,status,created_at)"
                + " VALUES ('owner-1','m-1','peer-1','peer-1','owner-1','旧消息','INCOMING','RECEIVED',1700000000000)"
        );
        legacy.execSQL(
            "INSERT INTO contacts (owner_id,user_id,display_name) VALUES ('owner-1','peer-1','Peer One')"
        );
        legacy.setVersion(1);
        legacy.close();

        // 打开当前版本的 helper，触发 onUpgrade。
        AndroidChatHistoryStore store = new AndroidChatHistoryStore(context, databaseName);
        List<RemoteIMMessage> messages =
            store.loadConversationPage("owner-1", "peer-1", null, null, 20).messages();

        assertEquals(1, messages.size());
        assertEquals("旧消息", messages.get(0).text());
        assertNull(messages.get(0).videoAttachment());
        assertEquals(1, store.loadContacts("owner-1").size());

        // 新表结构可用：写一条带视频的消息并读回。
        store.upsertMessage("owner-1", new RemoteIMMessage(
            "m-2",
            "remote-2",
            "peer-1",
            "owner-1",
            "[视频消息 12s]",
            RemoteIMMessage.Direction.INCOMING,
            RemoteIMMessage.Status.RECEIVED,
            1700000001000L,
            null,
            null,
            null,
            new RemoteIMVideoAttachment("/tmp/clip.mp4", "/tmp/cover.jpg", 12, 1080, 1920, 4194304L),
            RemoteIMOrigin.HUMAN
        ));

        List<RemoteIMMessage> afterInsert =
            store.loadConversationPage("owner-1", "peer-1", null, null, 20).messages();
        RemoteIMVideoAttachment video =
            afterInsert.get(afterInsert.size() - 1).videoAttachment();
        assertNotNull(video);
        assertEquals("/tmp/clip.mp4", video.localPath());
        assertEquals("/tmp/cover.jpg", video.coverPath());
        assertEquals(12, video.durationSeconds());
        assertEquals(1080, video.width());
        assertEquals(1920, video.height());
        assertEquals(4194304L, video.sizeBytes());
    }
}
