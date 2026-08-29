package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import android.content.Context;
import android.database.sqlite.SQLiteDatabase;

import androidx.test.core.app.ApplicationProvider;

import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;

import java.util.List;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class AndroidContactGroupMigrationTest {
    @Test
    public void v3ContactsMigrateWithoutLossAndGroupOperationsRemainAccountScoped() {
        Context context = ApplicationProvider.getApplicationContext();
        String databaseName = "maichat-contact-groups-robolectric.db";
        context.deleteDatabase(databaseName);

        SQLiteDatabase legacy = context.openOrCreateDatabase(
            databaseName,
            Context.MODE_PRIVATE,
            null
        );
        legacy.execSQL(
            "CREATE TABLE contacts ("
                + "owner_id TEXT NOT NULL,"
                + "user_id TEXT NOT NULL,"
                + "display_name TEXT NOT NULL,"
                + "avatar_url TEXT NOT NULL DEFAULT '',"
                + "PRIMARY KEY(owner_id, user_id))"
        );
        legacy.execSQL(
            "INSERT INTO contacts(owner_id,user_id,display_name) "
                + "VALUES('owner-1','peer-1','旧联系人')"
        );
        // v3 的真实库一定已有 messages；把前四版的列完整列出，才能同时验证
        // v3→v4 联系人分组与 v4→v5 caption_above 两段迁移都不重建历史。
        legacy.execSQL(
            "CREATE TABLE messages ("
                + "owner_id TEXT NOT NULL,id TEXT NOT NULL,remote_id TEXT NOT NULL DEFAULT '',"
                + "peer_id TEXT NOT NULL,from_id TEXT NOT NULL,to_id TEXT NOT NULL,"
                + "body TEXT NOT NULL,direction TEXT NOT NULL,status TEXT NOT NULL,created_at INTEGER NOT NULL,"
                + "image_path TEXT NOT NULL DEFAULT '',image_width INTEGER NOT NULL DEFAULT 0,"
                + "image_height INTEGER NOT NULL DEFAULT 0,image_size INTEGER NOT NULL DEFAULT 0,"
                + "voice_path TEXT NOT NULL DEFAULT '',voice_duration INTEGER NOT NULL DEFAULT 0,"
                + "file_path TEXT NOT NULL DEFAULT '',file_name TEXT NOT NULL DEFAULT '',"
                + "file_mime TEXT NOT NULL DEFAULT '',file_size INTEGER NOT NULL DEFAULT 0,"
                + "origin TEXT NOT NULL DEFAULT 'human',video_path TEXT NOT NULL DEFAULT '',"
                + "video_cover_path TEXT NOT NULL DEFAULT '',video_duration INTEGER NOT NULL DEFAULT 0,"
                + "video_width INTEGER NOT NULL DEFAULT 0,video_height INTEGER NOT NULL DEFAULT 0,"
                + "video_size INTEGER NOT NULL DEFAULT 0,approval_request_token TEXT NOT NULL DEFAULT '',"
                + "approval_request_actions TEXT NOT NULL DEFAULT '',approval_decision_token TEXT NOT NULL DEFAULT '',"
                + "approval_decision_action TEXT NOT NULL DEFAULT '',PRIMARY KEY(owner_id,id))"
        );
        legacy.execSQL(
            "INSERT INTO messages(owner_id,id,peer_id,from_id,to_id,body,direction,status,created_at) "
                + "VALUES('owner-1','legacy-1','peer-1','peer-1','owner-1','旧消息','INCOMING','RECEIVED',100)"
        );
        legacy.setVersion(3);
        legacy.close();

        AndroidChatHistoryStore store = new AndroidChatHistoryStore(context, databaseName);
        List<RemoteIMContact> migrated = store.loadContacts("owner-1");
        assertEquals(1, migrated.size());
        assertEquals("旧联系人", migrated.get(0).displayName());
        assertEquals("", migrated.get(0).groupName());
        List<RemoteIMMessage> legacyMessages =
            store.loadConversationPage("owner-1", "peer-1", null, null, 20).messages();
        assertEquals(1, legacyMessages.size());
        assertFalse(legacyMessages.get(0).captionAbove());

        RemoteIMMessage captioned = new RemoteIMMessage(
            "caption-1", "remote-caption-1", "peer-1", "owner-1", "说明在上面",
            RemoteIMMessage.Direction.INCOMING, RemoteIMMessage.Status.RECEIVED, 200L,
            null, null, null, null, RemoteIMOrigin.HUMAN
        );
        captioned.setCaptionAbove(true);
        store.upsertMessage("owner-1", captioned);
        assertTrue(
            store.loadConversationPage("owner-1", "peer-1", null, null, 20)
                .messages().get(1).captionAbove()
        );

        assertTrue(store.createContactGroup("owner-1", "同事"));
        assertTrue(store.createContactGroup("owner-1", "未分组"));
        assertTrue(store.createContactGroup("owner-2", "同事"));
        assertFalse(store.createContactGroup("owner-1", " 同事 "));
        assertTrue(store.setContactGroup("owner-1", "peer-1", "同事"));
        store.upsertContact("owner-1", new RemoteIMContact(
            "peer-1",
            "新资料",
            "https://example.com/avatar.png"
        ));
        assertEquals("同事", store.loadContacts("owner-1").get(0).groupName());
        assertEquals("新资料", store.loadContacts("owner-1").get(0).displayName());
        assertEquals(List.of("同事", "未分组"), store.loadContactGroups("owner-1"));
        assertEquals(List.of("同事"), store.loadContactGroups("owner-2"));

        // 目标名冲突时整个改名事务回滚，绝不把两个组悄悄合并。
        assertFalse(store.renameContactGroup("owner-1", "同事", "未分组"));
        assertEquals("同事", store.loadContacts("owner-1").get(0).groupName());
        assertTrue(store.renameContactGroup("owner-1", "同事", "工作"));
        assertEquals("工作", store.loadContacts("owner-1").get(0).groupName());

        // 删除分组只清关系，不删除联系人。
        assertTrue(store.deleteContactGroup("owner-1", "工作"));
        assertEquals(1, store.loadContacts("owner-1").size());
        assertEquals("", store.loadContacts("owner-1").get(0).groupName());
        store.close();

        // 重新打开后仍是 v5 数据，证明迁移与后续写入都已真正落库。
        AndroidChatHistoryStore reopened = new AndroidChatHistoryStore(context, databaseName);
        assertEquals(List.of("未分组"), reopened.loadContactGroups("owner-1"));
        assertEquals(1, reopened.loadContacts("owner-1").size());
        assertEquals("", reopened.loadContacts("owner-1").get(0).groupName());
        reopened.close();
        context.deleteDatabase(databaseName);
    }
}
