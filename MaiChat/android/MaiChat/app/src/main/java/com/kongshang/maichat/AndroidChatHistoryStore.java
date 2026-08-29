package com.kongshang.maichat;

import android.content.ContentValues;
import android.content.Context;
import android.database.Cursor;
import android.database.sqlite.SQLiteDatabase;
import android.database.sqlite.SQLiteOpenHelper;
import android.util.Log;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

public final class AndroidChatHistoryStore extends SQLiteOpenHelper {
    private static final String DATABASE_NAME = "maichat-history.db";
    private static final int DATABASE_VERSION = 5;
    private static final String TAG = "MaiChat.im";

    private final RemoteIMMediaPaths mediaPaths;

    public static final class Page {
        private final List<RemoteIMMessage> messages;
        private final boolean hasEarlier;

        Page(List<RemoteIMMessage> messages, boolean hasEarlier) {
            this.messages = Collections.unmodifiableList(messages);
            this.hasEarlier = hasEarlier;
        }

        public List<RemoteIMMessage> messages() {
            return messages;
        }

        public boolean hasEarlier() {
            return hasEarlier;
        }
    }

    public AndroidChatHistoryStore(Context context) {
        this(context, DATABASE_NAME);
    }

    AndroidChatHistoryStore(Context context, String databaseName) {
        super(context.getApplicationContext(), databaseName, null, DATABASE_VERSION);
        // 媒体路径入库存相对形式，读出来再按当前根还原：数据目录换根时历史不会整片失效。
        this.mediaPaths = RemoteIMMediaPaths.forApp(context);
    }

    @Override
    public void onCreate(SQLiteDatabase database) {
        database.execSQL(
            "CREATE TABLE contacts ("
                + "owner_id TEXT NOT NULL,"
                + "user_id TEXT NOT NULL,"
                + "display_name TEXT NOT NULL,"
                + "avatar_url TEXT NOT NULL DEFAULT '',"
                + "group_name TEXT NOT NULL DEFAULT '',"
                + "PRIMARY KEY(owner_id, user_id))"
        );
        createContactGroupsTable(database);
        database.execSQL(
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
                + "video_path TEXT NOT NULL DEFAULT '',"
                + "video_cover_path TEXT NOT NULL DEFAULT '',"
                + "video_duration INTEGER NOT NULL DEFAULT 0,"
                + "video_width INTEGER NOT NULL DEFAULT 0,"
                + "video_height INTEGER NOT NULL DEFAULT 0,"
                + "video_size INTEGER NOT NULL DEFAULT 0,"
                + "approval_request_token TEXT NOT NULL DEFAULT '',"
                + "approval_request_actions TEXT NOT NULL DEFAULT '',"
                + "approval_decision_token TEXT NOT NULL DEFAULT '',"
                + "approval_decision_action TEXT NOT NULL DEFAULT '',"
                + "caption_above INTEGER NOT NULL DEFAULT 0,"
                + "PRIMARY KEY(owner_id, id))"
        );
        database.execSQL(
            "CREATE INDEX messages_conversation_idx "
                + "ON messages(owner_id, peer_id, created_at DESC, id DESC)"
        );
        database.execSQL(
            "CREATE INDEX messages_remote_idx ON messages(owner_id, remote_id)"
        );
    }

    @Override
    public void onUpgrade(SQLiteDatabase database, int oldVersion, int newVersion) {
        if (oldVersion == newVersion) return;
        int version = oldVersion;
        if (version < 2) {
            addVideoColumns(database);
            version = 2;
        }
        if (version < 3) {
            addApprovalColumns(database);
            version = 3;
        }
        if (version < 4) {
            database.execSQL("ALTER TABLE contacts ADD COLUMN group_name TEXT NOT NULL DEFAULT ''");
            createContactGroupsTable(database);
            version = 4;
        }
        if (version < 5) {
            database.execSQL(
                "ALTER TABLE messages ADD COLUMN caption_above INTEGER NOT NULL DEFAULT 0"
            );
            version = 5;
        }
        if (version != newVersion) {
            // 只有在没有可用迁移路径时才重建。重建会清空用户本地聊天记录，
            // 为了加几列而删历史，这个代价不该由用户承担，所以放在最后一步。
            database.execSQL("DROP TABLE IF EXISTS messages");
            database.execSQL("DROP TABLE IF EXISTS contacts");
            onCreate(database);
        }
    }

    private static void addVideoColumns(SQLiteDatabase database) {
        database.execSQL("ALTER TABLE messages ADD COLUMN video_path TEXT NOT NULL DEFAULT ''");
        database.execSQL(
            "ALTER TABLE messages ADD COLUMN video_cover_path TEXT NOT NULL DEFAULT ''"
        );
        database.execSQL("ALTER TABLE messages ADD COLUMN video_duration INTEGER NOT NULL DEFAULT 0");
        database.execSQL("ALTER TABLE messages ADD COLUMN video_width INTEGER NOT NULL DEFAULT 0");
        database.execSQL("ALTER TABLE messages ADD COLUMN video_height INTEGER NOT NULL DEFAULT 0");
        database.execSQL("ALTER TABLE messages ADD COLUMN video_size INTEGER NOT NULL DEFAULT 0");
    }

    private static void addApprovalColumns(SQLiteDatabase database) {
        database.execSQL(
            "ALTER TABLE messages ADD COLUMN approval_request_token TEXT NOT NULL DEFAULT ''"
        );
        database.execSQL(
            "ALTER TABLE messages ADD COLUMN approval_request_actions TEXT NOT NULL DEFAULT ''"
        );
        database.execSQL(
            "ALTER TABLE messages ADD COLUMN approval_decision_token TEXT NOT NULL DEFAULT ''"
        );
        database.execSQL(
            "ALTER TABLE messages ADD COLUMN approval_decision_action TEXT NOT NULL DEFAULT ''"
        );
    }

    private static void createContactGroupsTable(SQLiteDatabase database) {
        database.execSQL(
            "CREATE TABLE IF NOT EXISTS contact_groups ("
                + "owner_id TEXT NOT NULL,"
                + "name TEXT NOT NULL,"
                + "sort_order INTEGER NOT NULL,"
                + "PRIMARY KEY(owner_id, name))"
        );
    }

    public List<String> loadContactGroups(String ownerId) {
        List<String> groups = new ArrayList<>();
        try (Cursor cursor = getReadableDatabase().query(
            "contact_groups",
            new String[]{"name"},
            "owner_id = ?",
            new String[]{clean(ownerId)},
            null,
            null,
            "sort_order ASC, name COLLATE NOCASE ASC"
        )) {
            while (cursor.moveToNext()) groups.add(cursor.getString(0));
        }
        return groups;
    }

    public List<RemoteIMContact> loadContacts(String ownerId) {
        List<RemoteIMContact> contacts = new ArrayList<>();
        String sql = "SELECT c.user_id,c.display_name,c.avatar_url,"
            + "CASE WHEN g.name IS NULL THEN '' ELSE c.group_name END "
            + "FROM contacts c LEFT JOIN contact_groups g "
            + "ON g.owner_id=c.owner_id AND g.name=c.group_name "
            + "WHERE c.owner_id=? ORDER BY c.display_name COLLATE NOCASE,c.user_id";
        try (Cursor cursor = getReadableDatabase().rawQuery(sql, new String[]{clean(ownerId)})) {
            while (cursor.moveToNext()) {
                contacts.add(new RemoteIMContact(
                    cursor.getString(0),
                    cursor.getString(1),
                    cursor.getString(2),
                    cursor.getString(3)
                ));
            }
        }
        return contacts;
    }

    public void upsertContact(String ownerId, RemoteIMContact contact) {
        String cleanOwnerId = clean(ownerId);
        ContentValues values = new ContentValues();
        values.put("display_name", contact.displayName());
        values.put("avatar_url", contact.avatarUrl());
        SQLiteDatabase database = getWritableDatabase();
        int updated = database.update(
            "contacts",
            values,
            "owner_id = ? AND user_id = ?",
            new String[]{cleanOwnerId, contact.userId()}
        );
        // 资料刷新只更新显示名和头像，不能用 REPLACE：REPLACE 会把本地 group_name
        // 一起重置。只有新联系人插入时才采纳其分组，且未知分组降级为空串。
        if (updated > 0) return;
        values.put("owner_id", cleanOwnerId);
        values.put("user_id", contact.userId());
        values.put("group_name", existingGroupName(database, cleanOwnerId, contact.groupName()));
        database.insertWithOnConflict("contacts", null, values, SQLiteDatabase.CONFLICT_IGNORE);
    }

    public boolean createContactGroup(String ownerId, String name) {
        String cleanOwnerId = clean(ownerId);
        String cleanName = ContactGroups.normalize(name);
        if (cleanOwnerId.isEmpty() || !ContactGroups.isAcceptableName(cleanName)) return false;
        SQLiteDatabase database = getWritableDatabase();
        long nextOrder = 0;
        try (Cursor cursor = database.rawQuery(
            "SELECT COALESCE(MAX(sort_order),-1)+1 FROM contact_groups WHERE owner_id=?",
            new String[]{cleanOwnerId}
        )) {
            if (cursor.moveToFirst()) nextOrder = cursor.getLong(0);
        }
        ContentValues values = new ContentValues();
        values.put("owner_id", cleanOwnerId);
        values.put("name", cleanName);
        values.put("sort_order", nextOrder);
        return database.insertWithOnConflict(
            "contact_groups", null, values, SQLiteDatabase.CONFLICT_IGNORE
        ) != -1;
    }

    public boolean renameContactGroup(String ownerId, String from, String to) {
        String cleanOwnerId = clean(ownerId);
        String oldName = ContactGroups.normalize(from);
        String newName = ContactGroups.normalize(to);
        if (cleanOwnerId.isEmpty() || !ContactGroups.isAcceptableName(newName)) return false;
        if (oldName.equals(newName)) return true;
        SQLiteDatabase database = getWritableDatabase();
        database.beginTransaction();
        try {
            ContentValues groupValues = new ContentValues();
            groupValues.put("name", newName);
            int changed = database.update(
                "contact_groups",
                groupValues,
                "owner_id = ? AND name = ?",
                new String[]{cleanOwnerId, oldName}
            );
            if (changed <= 0) return false;
            ContentValues contactValues = new ContentValues();
            contactValues.put("group_name", newName);
            database.update(
                "contacts",
                contactValues,
                "owner_id = ? AND group_name = ?",
                new String[]{cleanOwnerId, oldName}
            );
            database.setTransactionSuccessful();
            return true;
        } catch (RuntimeException error) {
            return false;
        } finally {
            database.endTransaction();
        }
    }

    public boolean deleteContactGroup(String ownerId, String name) {
        String cleanOwnerId = clean(ownerId);
        String cleanName = ContactGroups.normalize(name);
        if (cleanOwnerId.isEmpty() || cleanName.isEmpty()) return false;
        SQLiteDatabase database = getWritableDatabase();
        database.beginTransaction();
        try {
            ContentValues values = new ContentValues();
            values.put("group_name", "");
            database.update(
                "contacts", values, "owner_id = ? AND group_name = ?",
                new String[]{cleanOwnerId, cleanName}
            );
            int removed = database.delete(
                "contact_groups", "owner_id = ? AND name = ?",
                new String[]{cleanOwnerId, cleanName}
            );
            if (removed <= 0) return false;
            database.setTransactionSuccessful();
            return true;
        } finally {
            database.endTransaction();
        }
    }

    public boolean setContactGroup(String ownerId, String userId, String groupName) {
        String cleanOwnerId = clean(ownerId);
        String cleanUserId = clean(userId);
        if (cleanOwnerId.isEmpty() || cleanUserId.isEmpty()) return false;
        SQLiteDatabase database = getWritableDatabase();
        ContentValues values = new ContentValues();
        values.put("group_name", existingGroupName(
            database, cleanOwnerId, ContactGroups.normalize(groupName)
        ));
        return database.update(
            "contacts", values, "owner_id = ? AND user_id = ?",
            new String[]{cleanOwnerId, cleanUserId}
        ) > 0;
    }

    private static String existingGroupName(
        SQLiteDatabase database,
        String ownerId,
        String groupName
    ) {
        if (groupName.isEmpty()) return "";
        try (Cursor cursor = database.query(
            "contact_groups", new String[]{"name"},
            "owner_id = ? AND name = ?", new String[]{ownerId, groupName},
            null, null, null, "1"
        )) {
            return cursor.moveToFirst() ? groupName : "";
        }
    }

    public void deleteContact(String ownerId, String userId) {
        SQLiteDatabase database = getWritableDatabase();
        database.beginTransaction();
        try {
            database.delete(
                "contacts",
                "owner_id = ? AND user_id = ?",
                new String[]{clean(ownerId), clean(userId)}
            );
            database.delete(
                "messages",
                "owner_id = ? AND peer_id = ?",
                new String[]{clean(ownerId), clean(userId)}
            );
            database.setTransactionSuccessful();
        } finally {
            database.endTransaction();
        }
    }

    public List<RemoteIMMessage> loadConversationSummaries(String ownerId) {
        List<RemoteIMMessage> messages = new ArrayList<>();
        String sql = "SELECT " + columns("m")
            + " FROM messages m"
            + " WHERE m.owner_id = ?"
            + " AND m.id = (SELECT i.id FROM messages i"
            + " WHERE i.owner_id = m.owner_id AND i.peer_id = m.peer_id"
            + " ORDER BY i.created_at DESC, i.id DESC LIMIT 1)"
            + " ORDER BY m.created_at ASC, m.id ASC";
        try (Cursor cursor = getReadableDatabase().rawQuery(sql, new String[]{clean(ownerId)})) {
            while (cursor.moveToNext()) messages.add(readMessage(cursor));
        }
        return messages;
    }

    public Page loadConversationPage(
        String ownerId,
        String peerId,
        Long beforeCreatedAt,
        String beforeMessageId,
        int limit
    ) {
        int safeLimit = Math.max(1, Math.min(limit, 200));
        ArrayList<String> arguments = new ArrayList<>();
        arguments.add(clean(ownerId));
        arguments.add(clean(peerId));
        StringBuilder where = new StringBuilder("owner_id = ? AND peer_id = ?");
        if (beforeCreatedAt != null && beforeMessageId != null) {
            where.append(" AND (created_at < ? OR (created_at = ? AND id < ?))");
            arguments.add(String.valueOf(beforeCreatedAt));
            arguments.add(String.valueOf(beforeCreatedAt));
            arguments.add(beforeMessageId);
        }
        List<RemoteIMMessage> descending = new ArrayList<>();
        try (Cursor cursor = getReadableDatabase().query(
            "messages",
            columnNames(),
            where.toString(),
            arguments.toArray(new String[0]),
            null,
            null,
            "created_at DESC, id DESC",
            String.valueOf(safeLimit + 1)
        )) {
            while (cursor.moveToNext()) descending.add(readMessage(cursor));
        }
        boolean hasEarlier = descending.size() > safeLimit;
        if (hasEarlier) descending.remove(descending.size() - 1);
        Collections.reverse(descending);
        return new Page(descending, hasEarlier);
    }

    public boolean containsRemoteId(String ownerId, String remoteId) {
        String cleanRemoteId = clean(remoteId);
        if (cleanRemoteId.isEmpty()) return false;
        try (Cursor cursor = getReadableDatabase().rawQuery(
            "SELECT 1 FROM messages WHERE owner_id = ? AND remote_id = ? LIMIT 1",
            new String[]{clean(ownerId), cleanRemoteId}
        )) {
            return cursor.moveToFirst();
        }
    }

    public void upsertMessage(String ownerId, RemoteIMMessage message) {
        String owner = clean(ownerId);
        String peerId = message.fromUserId().equals(owner)
            ? message.toUserId()
            : message.fromUserId();
        ContentValues values = new ContentValues();
        values.put("owner_id", owner);
        values.put("id", message.id());
        values.put("remote_id", message.remoteId());
        values.put("peer_id", peerId);
        values.put("from_id", message.fromUserId());
        values.put("to_id", message.toUserId());
        values.put("body", message.text());
        values.put("direction", message.direction().name());
        values.put("status", message.status().name());
        values.put("created_at", message.createdAtMillis());
        RemoteIMImageAttachment image = message.imageAttachment();
        values.put("image_path", image == null ? "" : storedMediaPath(image.localPath()));
        values.put("image_width", image == null ? 0 : image.width());
        values.put("image_height", image == null ? 0 : image.height());
        values.put("image_size", image == null ? 0 : image.sizeBytes());
        RemoteIMVoiceAttachment voice = message.voiceAttachment();
        values.put("voice_path", voice == null ? "" : storedMediaPath(voice.localPath()));
        values.put("voice_duration", voice == null ? 0 : voice.durationSeconds());
        RemoteIMFileAttachment file = message.fileAttachment();
        values.put("file_path", file == null ? "" : storedMediaPath(file.localPath()));
        values.put("file_name", file == null ? "" : file.fileName());
        values.put("file_mime", file == null ? "" : file.mimeType());
        values.put("file_size", file == null ? 0 : file.sizeBytes());
        RemoteIMVideoAttachment video = message.videoAttachment();
        values.put("video_path", video == null ? "" : storedMediaPath(video.localPath()));
        values.put("video_cover_path", video == null ? "" : storedMediaPath(video.coverPath()));
        values.put("video_duration", video == null ? 0 : video.durationSeconds());
        values.put("video_width", video == null ? 0 : video.width());
        values.put("video_height", video == null ? 0 : video.height());
        values.put("video_size", video == null ? 0 : video.sizeBytes());
        RemoteIMApprovalRequest approvalRequest = message.approvalRequest();
        values.put(
            "approval_request_token",
            approvalRequest == null ? "" : approvalRequest.token()
        );
        values.put(
            "approval_request_actions",
            approvalRequest == null ? "" : approvalActions(approvalRequest.actions())
        );
        RemoteIMApprovalDecision approvalDecision = message.approvalDecision();
        values.put(
            "approval_decision_token",
            approvalDecision == null ? "" : approvalDecision.token()
        );
        values.put(
            "approval_decision_action",
            approvalDecision == null ? "" : approvalDecision.action().wireValue()
        );
        values.put("caption_above", message.captionAbove() ? 1 : 0);
        values.put("origin", message.origin().wireValue());
        getWritableDatabase().insertWithOnConflict(
            "messages",
            null,
            values,
            SQLiteDatabase.CONFLICT_REPLACE
        );
    }

    public void deleteConversation(String ownerId, String peerId) {
        getWritableDatabase().delete(
            "messages",
            "owner_id = ? AND peer_id = ?",
            new String[]{clean(ownerId), clean(peerId)}
        );
    }

    private static String[] columnNames() {
        return new String[]{
            "id", "remote_id", "from_id", "to_id", "body", "direction", "status",
            "created_at", "image_path", "image_width", "image_height", "image_size",
            "voice_path", "voice_duration", "file_path", "file_name", "file_mime",
            "file_size", "origin",
            "video_path", "video_cover_path", "video_duration", "video_width",
            "video_height", "video_size", "approval_request_token",
            "approval_request_actions", "approval_decision_token",
            "approval_decision_action", "caption_above"
        };
    }

    private static String columns(String alias) {
        StringBuilder result = new StringBuilder();
        String prefix = alias == null || alias.isEmpty() ? "" : alias + ".";
        for (String column : columnNames()) {
            if (result.length() > 0) result.append(',');
            result.append(prefix).append(column);
        }
        return result.toString();
    }

    /**
     * 媒体路径入库前压成相对形式。落在媒体根之外的不入库——但静默丢附件极难排查，
     * 所以留一行日志。正常收发的媒体都由 RemoteIMMediaPaths 分配位置，不会走到这里。
     */
    private String storedMediaPath(String runtimePath) {
        String stored = mediaPaths.toStoredPath(runtimePath);
        if (stored.isEmpty() && runtimePath != null && !runtimePath.trim().isEmpty()) {
            Log.w(TAG, "history: media path outside the media root was dropped."
                + " path=" + runtimePath
                + " <- attachment will not be persisted");
        }
        return stored;
    }

    private RemoteIMMessage readMessage(Cursor cursor) {
        RemoteIMImageAttachment image = null;
        String imagePath = mediaPaths.toAbsolutePath(cursor.getString(8));
        if (!imagePath.isEmpty()) {
            image = new RemoteIMImageAttachment(
                imagePath,
                cursor.getInt(9),
                cursor.getInt(10),
                cursor.getLong(11)
            );
        }
        RemoteIMVoiceAttachment voice = null;
        String voicePath = mediaPaths.toAbsolutePath(cursor.getString(12));
        if (!voicePath.isEmpty()) {
            voice = new RemoteIMVoiceAttachment(voicePath, cursor.getInt(13));
        }
        RemoteIMFileAttachment file = null;
        String filePath = mediaPaths.toAbsolutePath(cursor.getString(14));
        if (!filePath.isEmpty()) {
            file = new RemoteIMFileAttachment(
                filePath,
                cursor.getString(15),
                cursor.getString(16),
                cursor.getLong(17)
            );
        }
        RemoteIMVideoAttachment video = null;
        String videoPath = mediaPaths.toAbsolutePath(cursor.getString(19));
        if (!videoPath.isEmpty()) {
            video = new RemoteIMVideoAttachment(
                videoPath,
                mediaPaths.toAbsolutePath(cursor.getString(20)),
                cursor.getInt(21),
                cursor.getInt(22),
                cursor.getInt(23),
                cursor.getLong(24)
            );
        }
        RemoteIMApprovalRequest approvalRequest = readApprovalRequest(
            cursor.getString(25),
            cursor.getString(26)
        );
        RemoteIMApprovalDecision approvalDecision = readApprovalDecision(
            cursor.getString(27),
            cursor.getString(28)
        );
        RemoteIMMessage message = new RemoteIMMessage(
            cursor.getString(0),
            cursor.getString(1),
            cursor.getString(2),
            cursor.getString(3),
            cursor.getString(4),
            RemoteIMMessage.Direction.valueOf(cursor.getString(5)),
            RemoteIMMessage.Status.valueOf(cursor.getString(6)),
            cursor.getLong(7),
            image,
            voice,
            file,
            video,
            RemoteIMOrigin.fromWireValue(cursor.getString(18)),
            approvalRequest,
            approvalDecision
        );
        message.setCaptionAbove(cursor.getInt(29) != 0);
        return message;
    }

    private static String approvalActions(List<RemoteIMApprovalAction> actions) {
        StringBuilder value = new StringBuilder();
        for (RemoteIMApprovalAction action : actions) {
            if (value.length() > 0) value.append(',');
            value.append(action.wireValue());
        }
        return value.toString();
    }

    private static RemoteIMApprovalRequest readApprovalRequest(String token, String rawActions) {
        String cleanToken = clean(token);
        String cleanActions = clean(rawActions);
        if (cleanToken.isEmpty() || cleanActions.isEmpty()) return null;
        List<RemoteIMApprovalAction> actions = new ArrayList<>();
        for (String rawAction : cleanActions.split(",", -1)) {
            RemoteIMApprovalAction action = RemoteIMApprovalAction.fromWireValue(rawAction);
            if (action == null) return null;
            actions.add(action);
        }
        try {
            return new RemoteIMApprovalRequest(cleanToken, actions);
        } catch (IllegalArgumentException error) {
            return null;
        }
    }

    private static RemoteIMApprovalDecision readApprovalDecision(String token, String rawAction) {
        String cleanToken = clean(token);
        RemoteIMApprovalAction action = RemoteIMApprovalAction.fromWireValue(rawAction);
        if (cleanToken.isEmpty() || action == null) return null;
        try {
            return new RemoteIMApprovalDecision(cleanToken, action);
        } catch (IllegalArgumentException error) {
            return null;
        }
    }

    private static String clean(String value) {
        return value == null ? "" : value.trim();
    }
}
