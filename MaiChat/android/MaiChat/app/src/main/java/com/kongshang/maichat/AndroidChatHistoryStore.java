package com.kongshang.maichat;

import android.content.ContentValues;
import android.content.Context;
import android.database.Cursor;
import android.database.sqlite.SQLiteDatabase;
import android.database.sqlite.SQLiteOpenHelper;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

public final class AndroidChatHistoryStore extends SQLiteOpenHelper {
    private static final String DATABASE_NAME = "maichat-history.db";
    private static final int DATABASE_VERSION = 1;

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
    }

    @Override
    public void onCreate(SQLiteDatabase database) {
        database.execSQL(
            "CREATE TABLE contacts ("
                + "owner_id TEXT NOT NULL,"
                + "user_id TEXT NOT NULL,"
                + "display_name TEXT NOT NULL,"
                + "avatar_url TEXT NOT NULL DEFAULT '',"
                + "PRIMARY KEY(owner_id, user_id))"
        );
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
        database.execSQL("DROP TABLE IF EXISTS messages");
        database.execSQL("DROP TABLE IF EXISTS contacts");
        onCreate(database);
    }

    public List<RemoteIMContact> loadContacts(String ownerId) {
        List<RemoteIMContact> contacts = new ArrayList<>();
        try (Cursor cursor = getReadableDatabase().query(
            "contacts",
            new String[]{"user_id", "display_name", "avatar_url"},
            "owner_id = ?",
            new String[]{clean(ownerId)},
            null,
            null,
            "display_name COLLATE NOCASE ASC, user_id ASC"
        )) {
            while (cursor.moveToNext()) {
                contacts.add(new RemoteIMContact(
                    cursor.getString(0),
                    cursor.getString(1),
                    cursor.getString(2)
                ));
            }
        }
        return contacts;
    }

    public void upsertContact(String ownerId, RemoteIMContact contact) {
        ContentValues values = new ContentValues();
        values.put("owner_id", clean(ownerId));
        values.put("user_id", contact.userId());
        values.put("display_name", contact.displayName());
        values.put("avatar_url", contact.avatarUrl());
        getWritableDatabase().insertWithOnConflict(
            "contacts",
            null,
            values,
            SQLiteDatabase.CONFLICT_REPLACE
        );
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
        values.put("image_path", image == null ? "" : image.localPath());
        values.put("image_width", image == null ? 0 : image.width());
        values.put("image_height", image == null ? 0 : image.height());
        values.put("image_size", image == null ? 0 : image.sizeBytes());
        RemoteIMVoiceAttachment voice = message.voiceAttachment();
        values.put("voice_path", voice == null ? "" : voice.localPath());
        values.put("voice_duration", voice == null ? 0 : voice.durationSeconds());
        RemoteIMFileAttachment file = message.fileAttachment();
        values.put("file_path", file == null ? "" : file.localPath());
        values.put("file_name", file == null ? "" : file.fileName());
        values.put("file_mime", file == null ? "" : file.mimeType());
        values.put("file_size", file == null ? 0 : file.sizeBytes());
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
            "file_size", "origin"
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

    private static RemoteIMMessage readMessage(Cursor cursor) {
        RemoteIMImageAttachment image = null;
        String imagePath = cursor.getString(8);
        if (!imagePath.isEmpty()) {
            image = new RemoteIMImageAttachment(
                imagePath,
                cursor.getInt(9),
                cursor.getInt(10),
                cursor.getLong(11)
            );
        }
        RemoteIMVoiceAttachment voice = null;
        String voicePath = cursor.getString(12);
        if (!voicePath.isEmpty()) {
            voice = new RemoteIMVoiceAttachment(voicePath, cursor.getInt(13));
        }
        RemoteIMFileAttachment file = null;
        String filePath = cursor.getString(14);
        if (!filePath.isEmpty()) {
            file = new RemoteIMFileAttachment(
                filePath,
                cursor.getString(15),
                cursor.getString(16),
                cursor.getLong(17)
            );
        }
        return new RemoteIMMessage(
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
            RemoteIMOrigin.fromWireValue(cursor.getString(18))
        );
    }

    private static String clean(String value) {
        return value == null ? "" : value.trim();
    }
}
