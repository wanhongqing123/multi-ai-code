#include "storage/LocalMessageDatabase.h"

#include "model/ContactGroups.h"

#include <QDir>
#include <QFileInfo>
#include <QPair>
#include <QSet>
#include <QStringList>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>

namespace {

// 联系人读取统一走这一句：LEFT JOIN 让「指向已不存在的分组」在读取时自动降级成
// 未分组，不必写库修数据，也不会因为一次删组失败就让联系人挂在幽灵分组上。
const auto kSelectContactsSql = QStringLiteral(
    "SELECT c.user_id, c.display_name, c.avatar_url,"
    "       CASE WHEN g.name IS NULL THEN '' ELSE c.group_name END "
    "FROM contacts c LEFT JOIN contact_groups g ON g.name = c.group_name "
    "ORDER BY c.user_id");

QString approvalActionsText(const QList<RemoteIMApprovalAction>& actions) {
    QStringList values;
    for (RemoteIMApprovalAction action : actions) {
        values.append(remoteIMApprovalActionWireName(action));
    }
    return values.join(QLatin1Char(','));
}

QList<RemoteIMApprovalAction> approvalActionsFromText(const QString& text) {
    QList<RemoteIMApprovalAction> actions;
    for (const QString& value : text.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        RemoteIMApprovalAction action;
        if (!remoteIMApprovalActionFromWireName(value, &action)) return {};
        actions.append(action);
    }
    return actions;
}

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
    message.captionAbove = query.value(QStringLiteral("caption_above")).toInt() != 0;
    // 引用块。digest 是判据而不是 msgId：msgId 允许缺失（本地消息没有服务端 ID），
    // 但没有摘要的引用块会渲染成空白，那比没有引用更难看，所以按摘要判定「有没有引用」。
    message.quote.msgId = query.value(QStringLiteral("quote_msg_id")).toString();
    message.quote.senderId = query.value(QStringLiteral("quote_sender")).toString();
    message.quote.digest = query.value(QStringLiteral("quote_digest")).toString();
    message.quote.kind = query.value(QStringLiteral("quote_kind")).toString();
    message.hasQuote = !message.quote.digest.isEmpty();
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
    message.hasVideo = query.value(QStringLiteral("has_video")).toInt() != 0;
    message.video = RemoteIMVideoAttachment{
        query.value(QStringLiteral("video_path")).toString(),
        query.value(QStringLiteral("video_name")).toString(),
        query.value(QStringLiteral("video_cover")).toString(),
        query.value(QStringLiteral("video_seconds")).toInt(),
        query.value(QStringLiteral("video_bytes")).toLongLong()
    };
    const QString approvalToken = query.value(QStringLiteral("approval_token")).toString();
    const QList<RemoteIMApprovalAction> approvalActions =
        approvalActionsFromText(query.value(QStringLiteral("approval_actions")).toString());
    message.approvalRequest = RemoteIMApprovalRequest{approvalToken, approvalActions};
    message.hasApprovalRequest = message.approvalRequest.isValid();
    if (!message.hasApprovalRequest && approvalActions.size() == 1) {
        message.approvalDecision = RemoteIMApprovalDecision{
            approvalToken,
            approvalActions.first()
        };
        message.hasApprovalDecision = message.approvalDecision.isValid();
    }
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
        "  display_name TEXT NOT NULL,"
        "  avatar_url   TEXT NOT NULL DEFAULT ''"
        ")"));
    bool hasAvatarUrl = false;
    query.exec(QStringLiteral("PRAGMA table_info(contacts)"));
    while (query.next()) {
        if (query.value(1).toString() == QStringLiteral("avatar_url")) {
            hasAvatarUrl = true;
            break;
        }
    }
    if (!hasAvatarUrl) {
        query.exec(QStringLiteral(
            "ALTER TABLE contacts ADD COLUMN avatar_url TEXT NOT NULL DEFAULT ''"));
    }
    // 分组同样按「PRAGMA 判存在再 ALTER」加列，不重建表——重建会丢已有联系人。
    bool hasGroupName = false;
    query.exec(QStringLiteral("PRAGMA table_info(contacts)"));
    while (query.next()) {
        if (query.value(1).toString() == QStringLiteral("group_name")) {
            hasGroupName = true;
            break;
        }
    }
    if (!hasGroupName) {
        query.exec(QStringLiteral(
            "ALTER TABLE contacts ADD COLUMN group_name TEXT NOT NULL DEFAULT ''"));
    }
    // 分组单独建表而不是从 contacts.group_name 里隐式推导：推导表示不出「空分组」，
    // 而用户新建一个分组、还没往里放人时，它必须看得见，否则「新建分组」看起来什么都没发生。
    query.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS contact_groups ("
        "  name       TEXT PRIMARY KEY,"
        "  sort_order INTEGER NOT NULL DEFAULT 0"
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
        "  file_path     TEXT, file_name TEXT, file_mime TEXT, file_bytes INTEGER,"
        "  has_video     INTEGER NOT NULL DEFAULT 0,"
        "  video_path    TEXT, video_name TEXT, video_cover TEXT,"
        "  video_seconds INTEGER, video_bytes INTEGER,"
        "  approval_token TEXT, approval_actions TEXT,"
        "  caption_above INTEGER NOT NULL DEFAULT 0,"
        "  quote_msg_id TEXT, quote_sender TEXT, quote_digest TEXT, quote_kind TEXT"
        ")"));
    // 老库（建于视频功能之前）没有这几列，CREATE TABLE IF NOT EXISTS 不会补。
    // 与 contacts.avatar_url 一样按 PRAGMA 判存在再 ALTER，重复启动无副作用。
    QSet<QString> messageColumns;
    query.exec(QStringLiteral("PRAGMA table_info(messages)"));
    while (query.next()) messageColumns.insert(query.value(1).toString());
    const QList<QPair<QString, QString>> optionalColumns{
        {QStringLiteral("has_video"), QStringLiteral("INTEGER NOT NULL DEFAULT 0")},
        {QStringLiteral("video_path"), QStringLiteral("TEXT")},
        {QStringLiteral("video_name"), QStringLiteral("TEXT")},
        {QStringLiteral("video_cover"), QStringLiteral("TEXT")},
        {QStringLiteral("video_seconds"), QStringLiteral("INTEGER")},
        {QStringLiteral("video_bytes"), QStringLiteral("INTEGER")},
        {QStringLiteral("caption_above"), QStringLiteral("INTEGER NOT NULL DEFAULT 0")},
        {QStringLiteral("approval_token"), QStringLiteral("TEXT")},
        {QStringLiteral("approval_actions"), QStringLiteral("TEXT")},
        {QStringLiteral("quote_msg_id"), QStringLiteral("TEXT")},
        {QStringLiteral("quote_sender"), QStringLiteral("TEXT")},
        {QStringLiteral("quote_digest"), QStringLiteral("TEXT")},
        {QStringLiteral("quote_kind"), QStringLiteral("TEXT")}
    };
    for (const auto& column : optionalColumns) {
        if (messageColumns.contains(column.first)) continue;
        query.exec(QStringLiteral("ALTER TABLE messages ADD COLUMN %1 %2")
                       .arg(column.first, column.second));
    }

    query.exec(QStringLiteral(
        "CREATE INDEX IF NOT EXISTS idx_messages_peer_time ON messages(peer, created_at)"));
}

void LocalMessageDatabase::loadInto(ChatState& state) const {
    if (!db_.isOpen()) return;

    QSqlQuery contactQuery(db_);
    contactQuery.exec(kSelectContactsSql);
    while (contactQuery.next()) {
        state.upsertContact(RemoteIMContact{
            contactQuery.value(0).toString(),
            contactQuery.value(1).toString(),
            contactQuery.value(2).toString(),
            contactQuery.value(3).toString()
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
    contactQuery.exec(kSelectContactsSql);
    while (contactQuery.next()) {
        state.upsertContact(RemoteIMContact{
            contactQuery.value(0).toString(),
            contactQuery.value(1).toString(),
            contactQuery.value(2).toString(),
            contactQuery.value(3).toString()
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
        "INSERT INTO contacts(user_id, display_name, avatar_url) VALUES(?, ?, ?) "
        "ON CONFLICT(user_id) DO UPDATE SET "
        "display_name = CASE "
        "  WHEN excluded.display_name = excluded.user_id AND contacts.display_name <> contacts.user_id "
        "  THEN contacts.display_name ELSE excluded.display_name END, "
        "avatar_url = CASE WHEN excluded.avatar_url = '' THEN contacts.avatar_url ELSE excluded.avatar_url END"));
    query.addBindValue(contact.userId);
    query.addBindValue(contact.displayName.isEmpty() ? contact.userId : contact.displayName);
    const QString avatarUrl = contact.avatarUrl.trimmed();
    query.addBindValue(avatarUrl.isEmpty() ? QStringLiteral("") : avatarUrl);
    query.exec();
}

void LocalMessageDatabase::removeMessagesForPeer(const QString& userId) {
    if (!db_.isOpen()) return;
    QSqlQuery deleteMessages(db_);
    deleteMessages.prepare(QStringLiteral("DELETE FROM messages WHERE peer = ?"));
    deleteMessages.addBindValue(userId);
    deleteMessages.exec();
}

QStringList LocalMessageDatabase::contactGroups() const {
    QStringList groups;
    if (!db_.isOpen()) return groups;
    QSqlQuery query(db_);
    // 二级按 name 排序：sort_order 撞车或断档时顺序也是稳定的，不会每次启动跳来跳去。
    query.exec(QStringLiteral("SELECT name FROM contact_groups ORDER BY sort_order, name"));
    while (query.next()) groups.append(query.value(0).toString());
    return groups;
}

bool LocalMessageDatabase::createContactGroup(const QString& name) {
    if (!db_.isOpen()) return false;
    const QString clean = ContactGroups::normalize(name);
    if (!ContactGroups::isAcceptableName(clean)) return false;
    QSqlQuery query(db_);
    // 新组排在末尾：创建顺序就是用户给出的顺序，无需再让他手工排一遍。
    query.prepare(QStringLiteral(
        "INSERT INTO contact_groups(name, sort_order) "
        "VALUES(?, (SELECT COALESCE(MAX(sort_order), -1) + 1 FROM contact_groups))"));
    query.addBindValue(clean);
    // 主键冲突即重名，exec 返回 false——不必先查一次。
    return query.exec();
}

bool LocalMessageDatabase::renameContactGroup(const QString& from, const QString& to) {
    if (!db_.isOpen()) return false;
    const QString oldName = ContactGroups::normalize(from);
    const QString newName = ContactGroups::normalize(to);
    if (!ContactGroups::isAcceptableName(newName)) return false;
    if (oldName == newName) return true;

    db_.transaction();
    QSqlQuery rename(db_);
    rename.prepare(QStringLiteral("UPDATE contact_groups SET name = ? WHERE name = ?"));
    rename.addBindValue(newName);
    rename.addBindValue(oldName);
    // 目标名已存在会撞主键；此时整笔回滚，绝不把两个组悄悄合并成一个。
    if (!rename.exec() || rename.numRowsAffected() <= 0) {
        db_.rollback();
        return false;
    }
    QSqlQuery move(db_);
    move.prepare(QStringLiteral("UPDATE contacts SET group_name = ? WHERE group_name = ?"));
    move.addBindValue(newName);
    move.addBindValue(oldName);
    if (!move.exec()) {
        db_.rollback();
        return false;
    }
    db_.commit();
    return true;
}

void LocalMessageDatabase::deleteContactGroup(const QString& name) {
    if (!db_.isOpen()) return;
    const QString clean = ContactGroups::normalize(name);
    if (clean.isEmpty()) return;

    // 先清成员再删组，且放在同一个事务里。顺序反过来的话，中途失败会留下一批
    // 指向已删分组的联系人；而删组顺手删人是那种一旦发生就无法挽回的事，
    // 所以这里只动 group_name，永远不碰 contacts 的行本身。
    db_.transaction();
    QSqlQuery clear(db_);
    clear.prepare(QStringLiteral("UPDATE contacts SET group_name = '' WHERE group_name = ?"));
    clear.addBindValue(clean);
    if (!clear.exec()) {
        db_.rollback();
        return;
    }
    QSqlQuery drop(db_);
    drop.prepare(QStringLiteral("DELETE FROM contact_groups WHERE name = ?"));
    drop.addBindValue(clean);
    if (!drop.exec()) {
        db_.rollback();
        return;
    }
    db_.commit();
}

void LocalMessageDatabase::setContactGroup(const QString& userId, const QString& groupName) {
    if (!db_.isOpen() || userId.trimmed().isEmpty()) return;
    const QString clean = ContactGroups::normalize(groupName);
    // 必须是「空但非 null」的 QString：默认构造的 QString 是 null，绑定进去写的是
    // SQL NULL，而 group_name 是 NOT NULL 列，整条 UPDATE 会被约束静默拒绝——
    // 表现就是「移出分组」点了没反应。upsertContact 里的 avatar_url 同理。
    QString target = QStringLiteral("");
    if (!clean.isEmpty()) {
        // 只认已存在的分组。写入一个不存在的组名等于凭空造组，
        // 而分组的存在与否只由 contact_groups 说了算。
        QSqlQuery exists(db_);
        exists.prepare(QStringLiteral("SELECT 1 FROM contact_groups WHERE name = ?"));
        exists.addBindValue(clean);
        if (exists.exec() && exists.next()) target = clean;
    }
    QSqlQuery query(db_);
    query.prepare(QStringLiteral("UPDATE contacts SET group_name = ? WHERE user_id = ?"));
    query.addBindValue(target);
    query.addBindValue(userId);
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
        "  has_file, file_path, file_name, file_mime, file_bytes,"
        "  has_video, video_path, video_name, video_cover, video_seconds, video_bytes,"
        "  approval_token, approval_actions, caption_above,"
        "  quote_msg_id, quote_sender, quote_digest, quote_kind"
        ") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"));
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
    query.addBindValue(message.hasVideo ? 1 : 0);
    query.addBindValue(message.video.localPath);
    query.addBindValue(message.video.fileName);
    query.addBindValue(message.video.coverPath);
    query.addBindValue(message.video.durationSeconds);
    query.addBindValue(message.video.sizeBytes);
    const QString approvalToken = message.hasApprovalRequest
        ? message.approvalRequest.token
        : message.hasApprovalDecision ? message.approvalDecision.token : QString();
    const QList<RemoteIMApprovalAction> approvalActions = message.hasApprovalRequest
        ? message.approvalRequest.actions
        : message.hasApprovalDecision
            ? QList<RemoteIMApprovalAction>{message.approvalDecision.action}
            : QList<RemoteIMApprovalAction>{};
    query.addBindValue(approvalToken);
    query.addBindValue(approvalActionsText(approvalActions));
    query.addBindValue(message.captionAbove ? 1 : 0);
    // 没有引用时写空串而不是默认构造的 QString：后者是 null，绑进去成 SQL NULL。
    // 这几列虽然可空，但读取侧统一按空串判定，混进 NULL 会让判据出现两种形状。
    query.addBindValue(message.hasQuote ? message.quote.msgId : QStringLiteral(""));
    query.addBindValue(message.hasQuote ? message.quote.senderId : QStringLiteral(""));
    query.addBindValue(message.hasQuote ? message.quote.digest : QStringLiteral(""));
    query.addBindValue(message.hasQuote ? message.quote.kind : QStringLiteral(""));
    if (!query.exec()) return false;
    const bool inserted = query.numRowsAffected() > 0;
    if (!inserted && message.createdAtMillis > 0) {
        QSqlQuery enrich(db_);
        enrich.prepare(QStringLiteral("UPDATE messages SET created_at = ? WHERE id = ?"));
        enrich.addBindValue(message.createdAtMillis);
        enrich.addBindValue(message.id);
        enrich.exec();
    }
    return inserted;
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

void LocalMessageDatabase::updateMessageTime(const QString& messageId, qint64 createdAtMillis) {
    if (!db_.isOpen() || messageId.isEmpty()) return;
    QSqlQuery query(db_);
    query.prepare(QStringLiteral("UPDATE messages SET created_at = ? WHERE id = ? AND ? > 0"));
    query.addBindValue(createdAtMillis);
    query.addBindValue(messageId);
    query.addBindValue(createdAtMillis);
    query.exec();
}
