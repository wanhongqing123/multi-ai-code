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

    // ---- 联系人分组 ----
    // 分组是纯本地概念（联系人本身就不跨端同步）。空串表示未分组。

    // 自定义分组，按用户排序。「未分组」不在其中——它不是一个真的分组。
    QStringList contactGroups() const;
    // 名字会先归一化。空名、保留名「未分组」、以及重名都返回 false 且不写库。
    bool createContactGroup(const QString& name);
    // 事务内改组名并把成员一起迁过去。目标名不合法或已存在则返回 false。
    bool renameContactGroup(const QString& from, const QString& to);
    // 事务内先把成员置为未分组再删组——绝不删联系人。
    void deleteContactGroup(const QString& name);
    // 把联系人移入分组；groupName 为空串即移出到未分组。
    // 分组不存在则当作未分组处理，不会凭空造出一个组。
    void setContactGroup(const QString& userId, const QString& groupName);
    // 删除联系人并级联删除与该 peer 的全部消息。
    void removeContactCascade(const QString& userId);
    // 仅删除与该 peer 的全部消息（联系人保留）。
    void removeMessagesForPeer(const QString& userId);

    // 以消息 id 为主键 INSERT OR IGNORE；返回是否真的插入（false=已存在，
    // 供 SDK 漫游合并去重）。peer 为会话对端（按方向由调用方计算）。
    bool insertMessageIfAbsent(const RemoteIMMessage& message, const QString& peer);
    void updateMessageStatus(const QString& messageId, RemoteIMMessageStatus status);
    void updateMessageTime(const QString& messageId, qint64 createdAtMillis);
    // 出站消息发送成功后把临时 UUID 主键换成 SDK 稳定 id；若稳定 id 已存在
    //（漫游先落库），删除旧临时行。
    void adoptMessageId(const QString& oldId, const QString& newId);

private:
    void migrate();

    QString connectionName_;
    QSqlDatabase db_;
};
