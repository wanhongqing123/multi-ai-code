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

import java.io.File;
import java.util.List;

@RunWith(AndroidJUnit4.class)
public class AndroidChatHistoryStoreInstrumentedTest {
    @Test
    public void contactGroupsAreAccountScopedTransactionalAndProfileSafe() {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        String databaseName = "maichat-contact-groups-test.db";
        context.deleteDatabase(databaseName);
        AndroidChatHistoryStore store = new AndroidChatHistoryStore(context, databaseName);

        assertTrue(store.createContactGroup("owner-1", "同事"));
        assertTrue(store.createContactGroup("owner-1", "未分组"));
        assertTrue(store.createContactGroup("owner-2", "同事"));
        assertFalse(store.createContactGroup("owner-1", " 同事 "));
        store.upsertContact("owner-1", new RemoteIMContact("peer-1", "Peer"));
        store.setContactGroup("owner-1", "peer-1", "同事");
        store.upsertContact("owner-1", new RemoteIMContact("peer-1", "新资料", "avatar"));

        assertEquals("同事", store.loadContacts("owner-1").get(0).groupName());
        assertEquals("新资料", store.loadContacts("owner-1").get(0).displayName());
        assertEquals(List.of("同事", "未分组"), store.loadContactGroups("owner-1"));
        assertEquals(List.of("同事"), store.loadContactGroups("owner-2"));

        assertFalse(store.renameContactGroup("owner-1", "同事", "未分组"));
        assertTrue(store.renameContactGroup("owner-1", "同事", "工作"));
        assertEquals("工作", store.loadContacts("owner-1").get(0).groupName());
        assertTrue(store.deleteContactGroup("owner-1", "工作"));
        assertEquals("", store.loadContacts("owner-1").get(0).groupName());
        assertEquals(1, store.loadContacts("owner-1").size());

        store.close();
        context.deleteDatabase(databaseName);
    }

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
    public void keepsExistingHistoryWhenVideoColumnsAreAdded() throws Exception {
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
                + "avatar_url TEXT NOT NULL DEFAULT '',"
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
        assertFalse(messages.get(0).captionAbove());
        assertEquals(1, store.loadContacts("owner-1").size());
        assertEquals("", store.loadContacts("owner-1").get(0).groupName());
        assertTrue(store.createContactGroup("owner-1", "同事"));
        assertTrue(store.setContactGroup("owner-1", "peer-1", "同事"));
        assertEquals("同事", store.loadContacts("owner-1").get(0).groupName());

        // 新表结构可用：写一条带视频的消息并读回。
        RemoteIMMediaPaths mediaPaths = RemoteIMMediaPaths.forApp(context);
        File videoFile = new File(
            mediaPaths.directory(RemoteIMMediaPaths.INCOMING, RemoteIMMediaPaths.VIDEOS),
            "clip.mp4"
        );
        File coverFile = new File(
            mediaPaths.directory(RemoteIMMediaPaths.INCOMING, RemoteIMMediaPaths.VIDEO_COVERS),
            "cover.jpg"
        );
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
            new RemoteIMVideoAttachment(
                videoFile.getAbsolutePath(),
                coverFile.getAbsolutePath(),
                12,
                1080,
                1920,
                4194304L
            ),
            RemoteIMOrigin.HUMAN
        ));

        List<RemoteIMMessage> afterInsert =
            store.loadConversationPage("owner-1", "peer-1", null, null, 20).messages();
        RemoteIMVideoAttachment video =
            afterInsert.get(afterInsert.size() - 1).videoAttachment();
        assertNotNull(video);
        assertEquals(videoFile.getAbsolutePath(), video.localPath());
        assertEquals(coverFile.getAbsolutePath(), video.coverPath());
        assertEquals(12, video.durationSeconds());
        assertEquals(1080, video.width());
        assertEquals(1920, video.height());
        assertEquals(4194304L, video.sizeBytes());

        RemoteIMApprovalRequest request = new RemoteIMApprovalRequest(
            "approval-database-1",
            List.of(RemoteIMApprovalAction.APPROVE_ONCE, RemoteIMApprovalAction.REJECT)
        );
        store.upsertMessage("owner-1", new RemoteIMMessage(
            "m-3",
            "remote-3",
            "peer-1",
            "owner-1",
            "需要审批",
            RemoteIMMessage.Direction.INCOMING,
            RemoteIMMessage.Status.RECEIVED,
            1700000002000L,
            null,
            null,
            null,
            null,
            RemoteIMOrigin.MACHINE,
            request,
            null
        ));
        store.upsertMessage("owner-1", new RemoteIMMessage(
            "m-4",
            "remote-4",
            "owner-1",
            "peer-1",
            RemoteIMApprovalAction.APPROVE_ONCE.decisionDisplayText(),
            RemoteIMMessage.Direction.OUTGOING,
            RemoteIMMessage.Status.SENT,
            1700000003000L,
            null,
            null,
            null,
            null,
            RemoteIMOrigin.HUMAN,
            null,
            new RemoteIMApprovalDecision(
                request.token(),
                RemoteIMApprovalAction.APPROVE_ONCE
            )
        ));

        List<RemoteIMMessage> withApproval =
            store.loadConversationPage("owner-1", "peer-1", null, null, 20).messages();
        assertEquals(request, withApproval.get(withApproval.size() - 2).approvalRequest());
        assertEquals(
            request.token(),
            withApproval.get(withApproval.size() - 1).approvalDecision().token()
        );
    }
}
