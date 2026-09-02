#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <memory>

#include "im/RemoteIMClient.h"
#include "model/ChatState.h"
#include "storage/LocalMessageDatabase.h"

class RemoteIMApplication final : public QObject {
    Q_OBJECT

public:
    // database 可空：不带本地库时行为与从前一致（仅内存 + SDK 漫游）。
    // 带库时构造即加载全部本地历史；收发消息即时落库；SDK 漫游拉取降级为
    // 补充源，按消息 id 去重合并。
    RemoteIMApplication(QString ownerUserId,
                        std::unique_ptr<RemoteIMClient> client,
                        std::unique_ptr<LocalMessageDatabase> database = nullptr,
                        QObject* parent = nullptr);

    const ChatState& chatState() const;
    ChatState& chatState();
    RemoteIMClient& client();
    bool isConnected() const;

    // 会话是否还有未加载进内存的更早消息（分页启动只载每会话最近一页）。
    bool hasEarlierMessages(const QString& peerId) const;
    // 向上翻一页：从本地库取更早消息并入内存，返回本次加载条数。
    int loadEarlierMessages(const QString& peerId);

    void connectToService(int sdkAppId, const QString& userSig);
    void addContact(const QString& userId, const QString& displayName);
    void deleteContact(const QString& userId);

    // ---- 联系人分组 ----
    // 库与内存一起改，成功后发 stateChanged()。名字不合法或重名返回 false。
    bool createContactGroup(const QString& name);
    bool renameContactGroup(const QString& from, const QString& to);
    // 成员回到未分组，联系人本身保留。
    void deleteContactGroup(const QString& name);
    void setContactGroup(const QString& userId, const QString& groupName);
    // 清空与该 peer 的聊天记录（内存 + 本地库），好友保留。纯本地操作，不走远端。
    void clearMessagesWith(const QString& userId);
    void selectPeer(const QString& userId);
    // quote 为可选引用：hasQuote 为假时与原来完全一致。
    void sendText(const QString& text,
                  const RemoteIMQuote& quote = RemoteIMQuote(),
                  bool hasQuote = false);
    // 群发：给每个收件人各发一条独立消息，进各自的会话——不是一条"群消息"。
    // 这样每个人的聊天记录都是完整的，收件人那边看到的也和平时的私聊消息毫无区别。
    // 返回实际发出的人数（去重、去空之后）。
    int broadcastText(const QStringList& peerIds, const QString& text);
    // 把一条既有消息作为全新的普通消息转发给指定联系人。不继承引用、审批和
    // 原消息 id；附件必须仍有可读的本地缓存。成功入队返回 true。
    bool forwardMessage(const RemoteIMMessage& source, const QString& peerId);
    void sendApprovalDecision(const QString& token,
                              RemoteIMApprovalAction action,
                              std::function<void(bool)> completion = {});
    // text 非空时，图片/文件与配文合并成「一条」消息发送（气泡内图上文下）。
    // captionAbove：配文在附件上方（用户在输入框里就是这么排的）还是下方。
    void sendImage(const QString& localPath, const QString& text = QString(), bool captionAbove = false);
    void sendFile(const QString& localPath, const QString& text = QString(), bool captionAbove = false);
    // 发送视频（mp4/mov）。时长/尺寸从容器里解，封面在这里生成——IM SDK 两样都要，
    // 且都不会自己算。
    void sendVideo(const QString& localPath, const QString& text = QString(), bool captionAbove = false);
    void sendVoicePlaceholder();

signals:
    void stateChanged();
    // 群发全部回执到齐后发出一次。failedPeerIds 为发送失败的人，
    // 部分失败必须让用户看见是"谁"没收到——只报一句"部分失败"没法补救。
    void broadcastFinished(int total, const QStringList& failedPeerIds);
    // 实时收到的、之前没入过库的入站消息。UI 据此弹系统通知。
    //
    // 应用层只判断「这是不是一条值得通知的新消息」，不判断「现在该不该打扰用户」——
    // 后者取决于窗口是否激活、当前开着谁的会话，那是 UI 才知道的事。
    void incomingMessageArrived(const QString& peerId, const RemoteIMMessage& message);
    // 纯会话切换只更新当前会话区域，避免重建通讯录、设置页和整个会话列表。
    void selectionChanged(const QString& peerId);
    void connectionChanged(bool connected);
    void errorMessage(const QString& message);
    // 远程桌面信令：借道 IM 文本通道，但不入库、不进消息列表、不计未读。
    // 由 UI 层转交给 RemoteDesktopController 处理。
    void remoteDesktopSignalReceived(const QString& fromUserId, const QString& text);

private:
    void markMessage(const QString& messageId, RemoteIMMessageStatus status);
    void bindClientSignals();
    // 漫游/历史（live=false，不计红点）与实时推送（live=true，新入站消息计红点）
    // 的统一入库入口：upsert 联系人、按 id 去重落库、并入内存。
    void ingestMessages(const QList<RemoteIMMessage>& messages, bool live);
    void persistMessage(const RemoteIMMessage& message);
    // 发送成功后用 SDK 稳定 id 替换本地临时 UUID（内存+库），返回生效的 id。
    QString adoptRemoteMessageId(const QString& localId, const QString& remoteMessageId);
    bool sendVideoTo(const QString& peerId,
                     const QString& localPath,
                     const QString& text,
                     bool captionAbove);

    ChatState state_;
    QHash<QString, bool> hasEarlierMessages_;
    std::unique_ptr<RemoteIMClient> client_;
    std::unique_ptr<LocalMessageDatabase> database_;
    bool connected_ = false;
};
