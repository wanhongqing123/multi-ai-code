#include "storage/LocalMessageDatabase.h"

#include <QDir>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>

namespace {

RemoteIMMessage messageFromQuery(const QSqlQuery& query) {
    RemoteIMMessage message;
    message.id = query.value(QStringLiteral("id")).toString();
    message.fromUserId = query.value(QStringLiteral("from_user")).toString();
    message.toUserId = query.value(QStringLiteral("to_user")).toString();
    message.direction = query.value(QStringLiteral("direction")).toInt() == 1
                            ? RemoteIMMessageDirection::Outgoing
                            : RemoteIMMessageDirection::Incoming;
    message.status = static_cast<RemoteIMMessageStatus>(query.value(QStringLiteral("status")).toInt());
    message.text = query.value(QStringLiteral("text")).toString();
    message.createdAtMillis = query.value(QStringLiteral("created_at")).toLongLong();
    message.hasImage = query.value(QStringLiteral("has_image")).toInt() != 0;
    message.image = RemoteIMImageAttachment{
        query.value(QStringLiteral("image_path")).toString(),
        query.value(QStringLiteral("image_w")).toInt(),
        query.value(QStringLiteral("image_h")).toInt(),
        query.value(QStringLiteral("image_bytes")).toLongLong()
    };
    message.hasVoice = query.value(QStringLiteral("has_voice")).toInt() != 0;
    message.voice = RemoteIMVoiceAttachment{
        query.value(QStringLiteral("voice_path")).toString(),
        query.value(QStringLiteral("voice_seconds")).toInt()
    };
    message.hasFile = query.value(QStringLiteral("has_file")).toInt() != 0;
    message.file = RemoteIMFileAttachment{
        query.value(QStringLiteral("file_path")).toString(),
        query.value(QStringLiteral("file_name")).toString(),
        query.value(QStringLiteral("file_mime")).toString(),
        query.value(QStringLiteral("file_bytes")).toLongLong()
    };
    return message;
}

}  // namespace

LocalMessageDatabase::LocalMessageDatabase(const QString& dbFilePath)
    : connectionName_(QStringLiteral("remote_im_messages_") + QUuid::createUuid().toString(QUuid::WithoutBraces)) {
    QDir().mkpath(QFileInfo(dbFilePath).absolutePath());
    db_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName_);
    db_.setDatabaseName(dbFilePath);
    if (db_.open()) migrate();
}

LocalMessageDatabase::~LocalMessageDatabase() {
    if (db_.isOpen()) db_.close();
    db_ = QSqlDatabase();  // 释放句柄，否则 removeDatabase 会警告"仍在使用"。
    QSqlDatabase::removeDatabase(connectionName_);
}

bool LocalMessageDatabase::isOpen() const {
    return db_.isOpen();
}

void LocalMessageDatabase::migrate() {
    QSqlQuery query(db_);
    query.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS contacts ("
        "  user_id      TEXT PRIMARY KEY,"
        "  display_name TEXT NOT NULL"
        ")"));
    query.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS messages ("
        "  id            TEXT PRIMARY KEY,"
        "  from_user     TEXT NOT NULL,"
        "  to_user       TEXT NOT NULL,"
        "  peer          TEXT NOT NULL,"
        "  direction     INTEGER NOT NULL,"
        "  status        INTEGER NOT NULL,"
        "  text          TEXT NOT NULL DEFAULT '',"
        "  created_at    INTEGER NOT NULL,"
        "  has_image     INTEGER NOT NULL DEFAULT 0,"
        "  image_path    TEXT, image_w INTEGER, image_h INTEGER, image_bytes INTEGER,"
        "  has_voice     INTEGER NOT NULL DEFAULT 0,"
        "  voice_path    TEXT, voice_seconds INTEGER,"
        "  has_file      INTEGER NOT NULL DEFAULT 0,"
        "  file_path     TEXT, file_name TEXT, file_mime TEXT, file_bytes INTEGER"
        ")"));
    query.exec(QStringLiteral(
        "CREATE INDEX IF NOT EXISTS idx_messages_peer_time ON messages(peer, created_at)"));
}

void LocalMessageDatabase::loadInto(ChatState& state) const {
    if (!db_.isOpen()) return;

    QSqlQuery contactQuery(db_);
    contactQuery.exec(QStringLiteral("SELECT user_id, display_name FROM contacts ORDER BY user_id"));
    while (contactQuery.next()) {
        state.upsertContact(RemoteIMContact{
            contactQuery.value(0).toString(),
            contactQuery.value(1).toString()
        });
    }

    QSqlQuery messageQuery(db_);
    messageQuery.exec(QStringLiteral("SELECT * FROM messages ORDER BY created_at, id"));
    while (messageQuery.next()) {
        state.appendMessageForRestore(messageFromQuery(messageQuery));
    }
}

QHash<QString, bool> LocalMessageDatabase::loadRecentInto(ChatState& state, int perPeerLimit) const {
    QHash<QString, bool> hasEarlier;
    if (!db_.isOpen() || perPeerLimit <= 0) return hasEarlier;

    QSqlQuery contactQuery(db_);
    contactQuery.exec(QStringLiteral("SELECT user_id, display_name FROM contacts ORDER BY user_id"));
    while (contactQuery.next()) {
        state.upsertContact(RemoteIMContact{
            contactQuery.value(0).toString(),
            contactQuery.value(1).toString()
        });
    }

    // 每会话总量：> perPeerLimit 即还有更早消息可翻。
    QSqlQuery countQuery(db_);
    countQuery.exec(QStringLiteral("SELECT peer, COUNT(*) FROM messages GROUP BY peer"));
    while (countQuery.next()) {
        hasEarlier.insert(countQuery.value(0).toString(), countQuery.value(1).toInt() > perPeerLimit);
    }

    // 窗口函数按会话取最近 N 条（Qt 内置 SQLite ≥3.25 支持）。
    QSqlQuery messageQuery(db_);
    messageQuery.prepare(QStringLiteral(
        "SELECT * FROM ("
        "  SELECT m.*, ROW_NUMBER() OVER (PARTITION BY peer ORDER BY created_at DESC, id DESC) AS rn"
        "  FROM messages m"
        ") WHERE rn <= ? ORDER BY created_at, id"));
    messageQuery.addBindValue(perPeerLimit);
    messageQuery.exec();
    while (messageQuery.next()) {
        state.appendMessageForRestore(messageFromQuery(messageQuery));
    }
    return hasEarlier;
}

QList<RemoteIMMessage> LocalMessageDatabase::loadMessagesBefore(const QString& peer,
                                                                qint64 beforeCreatedAt,
                                                                const QString& beforeId,
                                                                int limit) const {
    QList<RemoteIMMessage> result;
    if (!db_.isOpen() || peer.isEmpty() || limit <= 0) return result;
    QSqlQuery query(db_);
    query.prepare(QStringLiteral(
        "SELECT * FROM messages"
        " WHERE peer = ? AND (created_at < ? OR (created_at = ? AND id < ?))"
        " ORDER BY created_at DESC, id DESC LIMIT ?"));
    query.addBindValue(peer);
    query.addBindValue(beforeCreatedAt);
    query.addBindValue(beforeCreatedAt);
    query.addBindValue(beforeId);
    query.addBindValue(limit);
    query.exec();
    while (query.next()) {
        result.prepend(messageFromQuery(query));  // DESC 取出，prepend 还原为升序
    }
    return result;
}

void LocalMessageDatabase::upsertContact(const RemoteIMContact& contact) {
    if (!db_.isOpen() || contact.userId.trimmed().isEmpty()) return;
    QSqlQuery query(db_);
    query.prepare(QStringLiteral(
        "INSERT INTO contacts(user_id, display_name) VALUES(?, ?) "
        "ON CONFLICT(user_id) DO UPDATE SET display_name = excluded.display_name"));
    query.addBindValue(contact.userId);
    query.addBindValue(contact.displayName.isEmpty() ? contact.userId : contact.displayName);
    query.exec();
}

void LocalMessageDatabase::removeContactCascade(const QString& userId) {
    if (!db_.isOpen()) return;
    QSqlQuery deleteMessages(db_);
    deleteMessages.prepare(QStringLiteral("DELETE FROM messages WHERE peer = ?"));
    deleteMessages.addBindValue(userId);
    deleteMessages.exec();

    QSqlQuery deleteContact(db_);
    deleteContact.prepare(QStringLiteral("DELETE FROM contacts WHERE user_id = ?"));
    deleteContact.addBindValue(userId);
    deleteContact.exec();
}

bool LocalMessageDatabase::insertMessageIfAbsent(const RemoteIMMessage& message, const QString& peer) {
    if (!db_.isOpen() || message.id.isEmpty()) return false;
    QSqlQuery query(db_);
    query.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO messages("
        "  id, from_user, to_user, peer, direction, status, text, created_at,"
        "  has_image, image_path, image_w, image_h, image_bytes,"
        "  has_voice, voice_path, voice_seconds,"
        "  has_file, file_path, file_name, file_mime, file_bytes"
        ") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"));
    query.addBindValue(message.id);
    query.addBindValue(message.fromUserId);
    query.addBindValue(message.toUserId);
    query.addBindValue(peer);
    query.addBindValue(message.direction == RemoteIMMessageDirection::Outgoing ? 1 : 0);
    query.addBindValue(static_cast<int>(message.status));
    query.addBindValue(message.text);
    query.addBindValue(message.createdAtMillis);
    query.addBindValue(message.hasImage ? 1 : 0);
    query.addBindValue(message.image.localPath);
    query.addBindValue(message.image.width);
    query.addBindValue(message.image.height);
    query.addBindValue(message.image.sizeBytes);
    query.addBindValue(message.hasVoice ? 1 : 0);
    query.addBindValue(message.voice.localPath);
    query.addBindValue(message.voice.durationSeconds);
    query.addBindValue(message.hasFile ? 1 : 0);
    query.addBindValue(message.file.localPath);
    query.addBindValue(message.file.fileName);
    query.addBindValue(message.file.mimeType);
    query.addBindValue(message.file.sizeBytes);
    if (!query.exec()) return false;
    return query.numRowsAffected() > 0;
}

void LocalMessageDatabase::adoptMessageId(const QString& oldId, const QString& newId) {
    if (!db_.isOpen() || oldId.isEmpty() || newId.isEmpty() || oldId == newId) return;
    QSqlQuery rename(db_);
    rename.prepare(QStringLiteral("UPDATE OR IGNORE messages SET id = ? WHERE id = ?"));
    rename.addBindValue(newId);
    rename.addBindValue(oldId);
    rename.exec();
    // 稳定 id 已存在（漫游先落库）时上面被 IGNORE，旧临时行是重复项，清掉。
    QSqlQuery cleanup(db_);
    cleanup.prepare(QStringLiteral("DELETE FROM messages WHERE id = ?"));
    cleanup.addBindValue(oldId);
    cleanup.exec();
}

void LocalMessageDatabase::updateMessageStatus(const QString& messageId, RemoteIMMessageStatus status) {
    if (!db_.isOpen()) return;
    QSqlQuery query(db_);
    query.prepare(QStringLiteral("UPDATE messages SET status = ? WHERE id = ?"));
    query.addBindValue(static_cast<int>(status));
    query.addBindValue(messageId);
    query.exec();
}
