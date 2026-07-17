#pragma once

#include <QHash>
#include <QList>
#include <QSqlDatabase>
#include <QString>

#include "model/ChatState.h"

// 本地消息库（SQLite，每账号一个库文件）。
//
// Desktop IM 的历史消息不能依赖腾讯 IM SDK 的漫游拉取（只保留最近几条），
// 因此所有通讯消息即时落库；登录后先从本库加载全部历史展示，SDK 漫游降级为
// 补充源（消息 id 主键天然去重，INSERT OR IGNORE 合并）。
class LocalMessageDatabase {
public:
    // dbFilePath: 库文件完整路径（父目录不存在会自动创建）。
    explicit LocalMessageDatabase(const QString& dbFilePath);
    ~LocalMessageDatabase();

    LocalMessageDatabase(const LocalMessageDatabase&) = delete;
    LocalMessageDatabase& operator=(const LocalMessageDatabase&) = delete;

    bool isOpen() const;

    // 联系人 + 全部消息（按 created_at 升序）恢复进 ChatState。
    void loadInto(ChatState& state) const;
    // 分页启动加载：联系人 + 每个会话最近 perPeerLimit 条。返回各会话是否还有
    // 更早的消息（供 UI 决定是否显示「加载更早」）。大历史下避免全量进内存。
    QHash<QString, bool> loadRecentInto(ChatState& state, int perPeerLimit) const;
    // 键集分页取更早的消息：严格早于 (beforeCreatedAt, beforeId)，按时间升序返回，
    // 最多 limit 条。
    QList<RemoteIMMessage> loadMessagesBefore(const QString& peer,
                                              qint64 beforeCreatedAt,
                                              const QString& beforeId,
                                              int limit) const;

    void upsertContact(const RemoteIMContact& contact);
    // 删除联系人并级联删除与该 peer 的全部消息。
    void removeContactCascade(const QString& userId);

    // 以消息 id 为主键 INSERT OR IGNORE；返回是否真的插入（false=已存在，
    // 供 SDK 漫游合并去重）。peer 为会话对端（按方向由调用方计算）。
    bool insertMessageIfAbsent(const RemoteIMMessage& message, const QString& peer);
    void updateMessageStatus(const QString& messageId, RemoteIMMessageStatus status);
    // 出站消息发送成功后把临时 UUID 主键换成 SDK 稳定 id；若稳定 id 已存在
    //（漫游先落库），删除旧临时行。
    void adoptMessageId(const QString& oldId, const QString& newId);

private:
    void migrate();

    QString connectionName_;
    QSqlDatabase db_;
};
