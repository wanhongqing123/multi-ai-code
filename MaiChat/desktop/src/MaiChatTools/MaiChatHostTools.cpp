// 文件作用：桌面端 Agent 工具；MaiChatHostTools 负责 Agent 工具注册、调用与结果返回，本文件给出其实现。
// 主要入口：unreadSummary。
// 直接依赖：MaiTool.h、app/RemoteIMApplication.h、model/MessageQuote.h、model/MessageSearch.h；实现与这些接口协作。
#include "MaiChatTools/MaiChatHostTools.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QMetaObject>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <memory>
#include <string>
#include <utility>

#include "MaiTool.h"
#include "app/RemoteIMApplication.h"
#include "model/MessageQuote.h"
#include "model/MessageSearch.h"

namespace {

enum class HostToolKind {
  ListContacts,
  RequestFriend,
  SetNickname,
  ListConversations,
  GetMessages,
  SearchMessages,
  GetUnreadSummary,
  SendText,
  SendMedia,
  ReplyMessage,
  BroadcastText,
  ListGroups,
  CreateGroup,
  GetGroupMessages,
  SendGroupText,
};

std::string toStdString(const QByteArray &bytes) {
  return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

MaiToolResult jsonResult(const QJsonObject &object) {
  return MaiToolResult::success(
      toStdString(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

MaiToolResult invalidInput(const QString &message) {
  return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                toStdString(message.toUtf8()));
}

bool parseObject(const std::string &arguments, QJsonObject *object,
                 QString *error) {
  QJsonParseError parseError;
  const QJsonDocument document = QJsonDocument::fromJson(
      QByteArray(arguments.data(), static_cast<int>(arguments.size())),
      &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
    if (error)
      *error = QStringLiteral("arguments must be a JSON object");
    return false;
  }
  if (object)
    *object = document.object();
  return true;
}

int boundedLimit(const QJsonObject &arguments, int fallback = 50) {
  return std::clamp(arguments.value(QStringLiteral("limit")).toInt(fallback), 1,
                    200);
}

QString messageKind(const RemoteIMMessage &message) {
  if (message.hasImage)
    return QStringLiteral("image");
  if (message.hasVideo)
    return QStringLiteral("video");
  if (message.hasVoice)
    return QStringLiteral("voice");
  if (message.hasFile)
    return QStringLiteral("file");
  return QStringLiteral("text");
}

QJsonObject messageJson(const RemoteIMMessage &message, const QString &peerId) {
  QJsonObject object;
  object.insert(QStringLiteral("id"), message.id);
  object.insert(QStringLiteral("peer_id"), peerId);
  object.insert(QStringLiteral("direction"),
                message.direction == RemoteIMMessageDirection::Incoming
                    ? QStringLiteral("incoming")
                    : QStringLiteral("outgoing"));
  object.insert(QStringLiteral("sender_id"), message.fromUserId);
  object.insert(QStringLiteral("text"), message.text);
  object.insert(QStringLiteral("kind"), messageKind(message));
  object.insert(QStringLiteral("created_at_ms"),
                QString::number(message.createdAtMillis));
  return object;
}

bool hasContact(const ChatState &state, const QString &peerId) {
  const QList<RemoteIMContact> contacts = state.contacts();
  return std::any_of(
      contacts.cbegin(), contacts.cend(),
      [&](const RemoteIMContact &contact) { return contact.userId == peerId; });
}

template <typename Action>
MaiToolResult onApplicationThread(const QPointer<RemoteIMApplication> &app,
                                  Action &&action) {
  if (app.isNull()) {
    return MaiToolResult::failure(MaiErrorCode::NotConfigured,
                                  "the MaiChat host is no longer available");
  }
  if (QThread::currentThread() == app->thread())
    return action(*app);

  MaiToolResult result = MaiToolResult::failure(
      MaiErrorCode::Internal,
      "failed to dispatch the tool to the MaiChat application thread");
  const bool invoked = QMetaObject::invokeMethod(
      app.data(),
      [&] {
        if (!app.isNull())
          result = action(*app);
      },
      Qt::BlockingQueuedConnection);
  if (!invoked || app.isNull()) {
    return MaiToolResult::failure(
        MaiErrorCode::Internal,
        "failed to access the MaiChat application state");
  }
  return result;
}

class MaiChatHostTool final : public MaiTool {
public:
  MaiChatHostTool(HostToolKind kind, const char *name, const char *description,
                  const char *schema, RemoteIMApplication &app, bool approval)
      : mKind(kind), mName(name), mDescription(description), mSchema(schema),
        mApp(&app), mApproval(approval) {}

  std::string name() const override { return mName; }
  std::string description() const override { return mDescription; }
  std::string parametersSchema() const override { return mSchema; }
  bool requiresApproval(const std::string &) const override {
    return mApproval;
  }
  bool requiresPerCallApproval(const std::string &) const override {
    return mApproval;
  }

  MaiToolResult execute(const std::string &argumentsJson,
                        const MaiToolContext &context) override {
    QJsonObject arguments;
    QString parseError;
    if (!parseObject(argumentsJson, &arguments, &parseError))
      return invalidInput(parseError);
    if ((mKind == HostToolKind::CreateGroup || mKind == HostToolKind::SendGroupText) &&
        context.isCanceled())
      return MaiToolResult::failure(MaiErrorCode::Canceled, "group operation was canceled");
    if (mKind == HostToolKind::SendMedia)
      return sendMedia(arguments, context);
    return onApplicationThread(mApp, [&](RemoteIMApplication &app) {
      switch (mKind) {
      case HostToolKind::ListContacts:
        return listContacts(app, arguments);
      case HostToolKind::RequestFriend:
        return requestFriend(app, arguments);
      case HostToolKind::SetNickname:
        return setNickname(app, arguments);
      case HostToolKind::ListConversations:
        return listConversations(app, arguments);
      case HostToolKind::GetMessages:
        return getMessages(app, arguments);
      case HostToolKind::SearchMessages:
        return searchMessages(app, arguments);
      case HostToolKind::GetUnreadSummary:
        return unreadSummary(app);
      case HostToolKind::SendText:
        return sendText(app, arguments);
      case HostToolKind::SendMedia:
        break;
      case HostToolKind::ReplyMessage:
        return replyMessage(app, arguments);
      case HostToolKind::BroadcastText:
        return broadcastText(app, arguments);
      case HostToolKind::ListGroups:
        return listGroups(app, arguments);
      case HostToolKind::CreateGroup:
        return createGroup(app, arguments);
      case HostToolKind::GetGroupMessages:
        return getGroupMessages(app, arguments);
      case HostToolKind::SendGroupText:
        return sendGroupText(app, arguments);
      }
      return MaiToolResult::failure(MaiErrorCode::Internal,
                                    "unknown MaiChat host tool");
    });
  }

private:
  MaiToolResult requestFriend(RemoteIMApplication &app,
                              const QJsonObject &arguments) const {
    const QString account = arguments.value(QStringLiteral("account")).toString().trimmed();
    static const QRegularExpression accountId(QStringLiteral("^[A-Za-z0-9_-]{1,128}$"));
    static const QRegularExpression phone(QStringLiteral("^(\\+86)?1[3-9][0-9]{9}$"));
    QString normalized = account;
    normalized.remove(QRegularExpression(QStringLiteral("[ -]")));
    if ((!accountId.match(account).hasMatch() && !phone.match(normalized).hasMatch()) ||
        account == app.chatState().ownerUserId())
      return invalidInput(QStringLiteral("account must be a phone or another MaiChat account ID"));
    if (!app.isConnected())
      return MaiToolResult::failure(MaiErrorCode::NotConfigured, "MaiChat IM is disconnected");
    app.requestFriend(account, account);
    return jsonResult(QJsonObject{{QStringLiteral("queued"), true},
                                  {QStringLiteral("account"), account}});
  }

  MaiToolResult setNickname(RemoteIMApplication &app,
                            const QJsonObject &arguments) const {
    const QJsonValue value = arguments.value(QStringLiteral("nickname"));
    if (!value.isString()) return invalidInput(QStringLiteral("nickname must be a string"));
    const QString nickname = value.toString().trimmed();
    if (nickname.toUcs4().size() > 32 || nickname.toUtf8().size() > 120)
      return invalidInput(QStringLiteral("nickname is too long"));
    if (!app.updateNickname(nickname, {}))
      return MaiToolResult::failure(MaiErrorCode::NotConfigured,
                                    "MaiChat account profile is unavailable");
    return jsonResult(QJsonObject{{QStringLiteral("queued"), true},
                                  {QStringLiteral("nickname"), nickname}});
  }

  // 校验联系人、媒体类型与本地文件，再通过宿主 IM 发送对应附件。
  // 只将真实发送结果回给 Agent，不能把文件存在等同于消息已交付。
  MaiToolResult sendMedia(const QJsonObject &arguments,
                          const MaiToolContext &context) const {
    const QString peerId = arguments.value(QStringLiteral("peer_id")).toString().trimmed();
    const QString requested = arguments.value(QStringLiteral("file_path")).toString().trimmed();
    const QString kind = arguments.value(QStringLiteral("type")).toString().trimmed();
    const QString caption = arguments.value(QStringLiteral("caption")).toString().trimmed();
    if (peerId.isEmpty() || requested.isEmpty() ||
        (kind != QStringLiteral("image") && kind != QStringLiteral("video") &&
         kind != QStringLiteral("audio"))) {
      return invalidInput(QStringLiteral("peer_id, file_path, and type=image|video|audio are required"));
    }
    const std::string path = context.resolvePath(toStdString(requested.toUtf8()));
    if (path.empty())
      return invalidInput(QStringLiteral("file_path is outside the accessible area"));
    const QFileInfo source(QString::fromUtf8(path.data(), static_cast<int>(path.size())));
    const qint64 limit = kind == QStringLiteral("image") ? 20LL * 1024 * 1024
                                                       : 1024LL * 1024 * 1024;
    if (!source.isFile() || !source.isReadable() || source.size() <= 0 ||
        source.size() > limit) {
      return invalidInput(QStringLiteral("media file is missing, unreadable, empty, or too large"));
    }
    if (kind == QStringLiteral("image") && !QImageReader(source.filePath()).canRead())
      return invalidInput(QStringLiteral("image file cannot be decoded"));
    if (kind == QStringLiteral("video") &&
        source.suffix().compare(QStringLiteral("mp4"), Qt::CaseInsensitive) != 0 &&
        source.suffix().compare(QStringLiteral("mov"), Qt::CaseInsensitive) != 0) {
      return invalidInput(QStringLiteral("video messages require mp4 or mov"));
    }
    const auto validatePeer = onApplicationThread(mApp, [&](RemoteIMApplication &app) {
      return hasContact(app.chatState(), peerId)
                 ? jsonResult({{QStringLiteral("valid"), true}})
                 : invalidInput(QStringLiteral("peer_id is not a MaiChat contact"));
    });
    if (validatePeer.hasError()) return validatePeer;

    const QString directory = QDir(QStandardPaths::writableLocation(
        QStandardPaths::AppDataLocation)).filePath(QStringLiteral("AgentMedia"));
    if (!QDir().mkpath(directory))
      return MaiToolResult::failure(MaiErrorCode::Internal,
                                    "cannot create MaiChat media storage");
    const QString copiedPath = QDir(directory).filePath(
        QUuid::createUuid().toString(QUuid::WithoutBraces) +
        QStringLiteral("-") + source.fileName());
    QFile input(source.filePath());
    QSaveFile output(copiedPath);
    if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly))
      return MaiToolResult::failure(MaiErrorCode::Internal,
                                    "cannot read or save media file");
    while (!input.atEnd()) {
      if (context.isCanceled()) {
        output.cancelWriting();
        return MaiToolResult::failure(MaiErrorCode::Canceled,
                                      "media send was canceled before queueing");
      }
      const QByteArray chunk = input.read(1024 * 1024);
      if (chunk.isEmpty() || output.write(chunk) != chunk.size()) {
        output.cancelWriting();
        return MaiToolResult::failure(MaiErrorCode::Internal,
                                      "failed to copy media into MaiChat storage");
      }
    }
    if (!output.commit())
      return MaiToolResult::failure(MaiErrorCode::Internal,
                                    "failed to finalize media in MaiChat storage");
    const auto result = onApplicationThread(mApp, [&](RemoteIMApplication &app) {
      if (!hasContact(app.chatState(), peerId))
        return invalidInput(QStringLiteral("peer_id is no longer a MaiChat contact"));
      const bool queued = kind == QStringLiteral("image")
          ? app.sendImageTo(peerId, copiedPath, caption)
          : kind == QStringLiteral("video")
              ? app.sendVideoTo(peerId, copiedPath, caption)
              : app.sendFileTo(peerId, copiedPath, caption);
      if (!queued)
        return MaiToolResult::failure(MaiErrorCode::Internal,
                                      "MaiChat did not queue the media message");
      return jsonResult({{QStringLiteral("queued"), true},
                         {QStringLiteral("peer_id"), peerId},
                         {QStringLiteral("type"), kind},
                         {QStringLiteral("caption_attached"), !caption.isEmpty()},
                         {QStringLiteral("message_kind"),
                          kind == QStringLiteral("audio") ? QStringLiteral("file") : kind},
                         {QStringLiteral("path"), copiedPath}});
    });
    if (result.hasError()) QFile::remove(copiedPath);
    return result;
  }

  static MaiToolResult listContacts(RemoteIMApplication &app,
                                    const QJsonObject &arguments) {
    const QString query =
        arguments.value(QStringLiteral("query")).toString().trimmed();
    const int limit = boundedLimit(arguments);
    QJsonArray contacts;
    for (const RemoteIMContact &contact : app.chatState().contacts()) {
      if (!query.isEmpty() &&
          !contact.userId.contains(query, Qt::CaseInsensitive) &&
          !contact.displayName.contains(query, Qt::CaseInsensitive)) {
        continue;
      }
      QJsonObject item;
      item.insert(QStringLiteral("user_id"), contact.userId);
      item.insert(QStringLiteral("display_name"), contact.displayName);
      item.insert(QStringLiteral("group"), contact.groupName);
      contacts.append(item);
      if (contacts.size() >= limit)
        break;
    }
    return jsonResult({{QStringLiteral("contacts"), contacts},
                       {QStringLiteral("count"), contacts.size()}});
  }

  static MaiToolResult listGroups(RemoteIMApplication &app,
                                  const QJsonObject &arguments) {
    const QString query = arguments.value(QStringLiteral("query")).toString().trimmed();
    const int limit = boundedLimit(arguments);
    QJsonArray values;
    for (const RemoteIMGroup& group : app.groups()) {
      if (!query.isEmpty() && !group.name.contains(query, Qt::CaseInsensitive) &&
          !group.groupId.contains(query, Qt::CaseInsensitive)) continue;
      values.append(QJsonObject{{QStringLiteral("group_id"), group.groupId},
                                {QStringLiteral("name"), group.name},
                                {QStringLiteral("member_count"), group.memberCount},
                                {QStringLiteral("pending_activation"), group.pendingActivation},
                                {QStringLiteral("unread_count"), app.groupUnreadCount(group.groupId)}});
      if (values.size() >= limit) break;
    }
    return jsonResult({{QStringLiteral("groups"), values},
                       {QStringLiteral("count"), values.size()}});
  }

  static MaiToolResult createGroup(RemoteIMApplication &app,
                                   const QJsonObject &arguments) {
    for (const RemoteIMGroup& group : app.groups()) {
      if (!group.pendingActivation) continue;
      return jsonResult({{QStringLiteral("created"), false},
                         {QStringLiteral("group_id"), group.groupId},
                         {QStringLiteral("name"), group.name},
                         {QStringLiteral("member_count"), group.memberCount},
                         {QStringLiteral("pending_activation"), true},
                         {QStringLiteral("next_step"), QStringLiteral(
                             "Open this existing group and send one text message; do not create it again.")}});
    }
    const QString name = arguments.value(QStringLiteral("name")).toString().trimmed();
    const QJsonArray rawMembers = arguments.value(QStringLiteral("member_ids")).toArray();
    if (name.isEmpty() || name.size() > 30 || name.toUtf8().size() > 100 || rawMembers.size() < 1 ||
        rawMembers.size() > 19)
      return invalidInput(QStringLiteral("name must be <=100 UTF-8 bytes and member_ids must contain 1..19 friends"));
    QStringList members;
    for (const QJsonValue& value : rawMembers) {
      if (!value.isString()) return invalidInput(QStringLiteral("member_ids must contain strings"));
      const QString id = value.toString().trimmed();
      if (id.isEmpty() || id == app.chatState().ownerUserId() || members.contains(id) ||
          !hasContact(app.chatState(), id) || !app.isAcceptedFriend(id))
        return invalidInput(QStringLiteral("member_ids must be distinct accepted friends other than self"));
      members.append(id);
    }
    if (!app.isConnected())
      return MaiToolResult::failure(MaiErrorCode::NotConfigured, "MaiChat IM is disconnected");
    struct Result {
      bool done = false;
      bool ok = false;
      QString error;
      RemoteIMGroup group;
    };
    auto result = std::make_shared<Result>();
    QEventLoop loop;
    QPointer<QEventLoop> waiting(&loop);
    app.createGroup(name, members,
        [result, waiting](bool ok, const QString& error, const RemoteIMGroup& group) {
      result->done = true;
      result->ok = ok;
      result->error = error;
      result->group = group;
      if (waiting) waiting->quit();
    });
    if (!result->done) {
      QTimer timer;
      timer.setSingleShot(true);
      QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
      timer.start(30000);
      loop.exec();
    }
    if (!result->done)
      return MaiToolResult::failure(MaiErrorCode::Internal,
                                    "group creation status is unknown; list groups before retrying");
    if (!result->ok && !result->group.groupId.isEmpty())
      return jsonResult({{QStringLiteral("created"), true},
                         {QStringLiteral("group_id"), result->group.groupId},
                         {QStringLiteral("name"), result->group.name},
                         {QStringLiteral("member_count"), result->group.memberCount},
                         {QStringLiteral("pending_activation"), true},
                         {QStringLiteral("next_step"), QStringLiteral(
                             "Send one text message to this group; do not create it again.")},
                         {QStringLiteral("error"), result->error}});
    if (!result->ok)
      return MaiToolResult::failure(MaiErrorCode::Internal,
          toStdString((result->error.isEmpty() ? QStringLiteral("group creation failed") :
                       result->error).toUtf8()));
    return jsonResult({{QStringLiteral("created"), true},
                       {QStringLiteral("group_id"), result->group.groupId},
                       {QStringLiteral("name"), result->group.name},
                       {QStringLiteral("member_count"), result->group.memberCount},
                       {QStringLiteral("pending_activation"), result->group.pendingActivation}});
  }

  static MaiToolResult getGroupMessages(RemoteIMApplication &app,
                                        const QJsonObject &arguments) {
    const QString id = arguments.value(QStringLiteral("group_id")).toString().trimmed();
    if (id.isEmpty()) return invalidInput(QStringLiteral("group_id is required"));
    const auto groups = app.groups();
    if (std::none_of(groups.cbegin(), groups.cend(), [&id](const auto& group) {
          return group.groupId == id;
        })) return invalidInput(QStringLiteral("group_id is not a joined MaiChat group"));
    if (!app.hasLoadedGroupHistory(id)) {
      if (!app.isConnected())
        return MaiToolResult::failure(MaiErrorCode::NotConfigured,
                                      "MaiChat IM is disconnected and group history is not loaded");
      QEventLoop loop;
      bool finished = false;
      bool succeeded = false;
      QObject::connect(&app, &RemoteIMApplication::groupHistoryFinished, &loop,
                       [&](const QString& groupId, bool ok) {
        if (groupId != id) return;
        finished = true;
        succeeded = ok;
        loop.quit();
      });
      app.ensureGroupHistory(id);
      if (!app.hasLoadedGroupHistory(id) && !finished) {
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        timeout.start(30000);
        loop.exec();
      }
      if (!app.hasLoadedGroupHistory(id) || (finished && !succeeded))
        return MaiToolResult::failure(MaiErrorCode::Internal,
                                      "group history is still loading or failed; retry this read later");
    }
    const int limit = std::clamp(arguments.value(QStringLiteral("limit")).toInt(20), 1, 20);
    const auto all = app.groupMessages(id);
    QJsonArray values;
    for (int i = qMax(0, all.size() - limit); i < all.size(); ++i) {
      const auto& message = all.at(i);
      values.append(QJsonObject{{QStringLiteral("id"), message.id},
                                {QStringLiteral("sender_id"), message.senderId},
                                {QStringLiteral("direction"),
                                 message.fromSelf ? QStringLiteral("outgoing") : QStringLiteral("incoming")},
                                {QStringLiteral("text"), message.text},
                                {QStringLiteral("remote_id"),
                                 message.pending || message.failed ? QString() : message.id},
                                {QStringLiteral("created_at_ms"),
                                 static_cast<double>(message.createdAtMillis)}});
    }
    return jsonResult({{QStringLiteral("group_id"), id},
                       {QStringLiteral("messages"), values},
                       {QStringLiteral("count"), values.size()}});
  }

  static MaiToolResult sendGroupText(RemoteIMApplication &app,
                                     const QJsonObject &arguments) {
    const QString id = arguments.value(QStringLiteral("group_id")).toString().trimmed();
    const QString text = arguments.value(QStringLiteral("text")).toString().trimmed();
    if (id.isEmpty() || text.isEmpty())
      return invalidInput(QStringLiteral("group_id and text are required"));
    QSet<QString> beforeIds;
    for (const auto& message : app.groupMessages(id)) beforeIds.insert(message.id);
    if (!app.sendGroupText(id, text))
      return MaiToolResult::failure(MaiErrorCode::NotConfigured,
                                    "MaiChat did not queue the group message");
    auto findQueued = [&]() {
      const auto messages = app.groupMessages(id);
      for (int index = messages.size() - 1; index >= 0; --index) {
        const auto& message = messages.at(index);
        if (message.fromSelf && message.text == text && !beforeIds.contains(message.id))
          return message;
      }
      return RemoteIMGroupMessage{};
    };
    RemoteIMGroupMessage message = findQueued();
    const QString queuedId = message.id;
    if (message.pending) {
      QEventLoop loop;
      QTimer timeout;
      timeout.setSingleShot(true);
      QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
      QObject::connect(&app, &RemoteIMApplication::groupsChanged, &loop, [&] {
        message = findQueued();
        if (!message.pending) loop.quit();
      });
      timeout.start(30000);
      loop.exec();
      message = findQueued();
    }
    if (message.failed)
      return MaiToolResult::failure(MaiErrorCode::Internal, "group message delivery failed");
    return jsonResult({{QStringLiteral("queued"), true},
                       {QStringLiteral("sent"), !message.pending && !message.id.isEmpty()},
                       {QStringLiteral("group_id"), id},
                       {QStringLiteral("message_id"),
                        message.id.isEmpty() ? queuedId : message.id},
                       {QStringLiteral("remote_id"),
                        message.pending ? QString() : message.id}});
  }

  static MaiToolResult listConversations(RemoteIMApplication &app,
                                         const QJsonObject &arguments) {
    struct Conversation {
      RemoteIMContact contact;
      RemoteIMMessage latest;
      bool hasLatest = false;
    };
    QList<Conversation> conversations;
    for (const RemoteIMContact &contact : app.chatState().contacts()) {
      Conversation item;
      item.contact = contact;
      item.hasLatest =
          app.chatState().latestMessageWith(contact.userId, &item.latest);
      if (item.hasLatest)
        conversations.append(item);
    }
    std::sort(conversations.begin(), conversations.end(),
              [](const Conversation &left, const Conversation &right) {
                return left.latest.createdAtMillis >
                       right.latest.createdAtMillis;
              });
    QJsonArray result;
    const int limit = boundedLimit(arguments);
    for (const Conversation &conversation : conversations) {
      QJsonObject item;
      item.insert(QStringLiteral("peer_id"), conversation.contact.userId);
      item.insert(QStringLiteral("display_name"),
                  conversation.contact.displayName);
      item.insert(QStringLiteral("unread"),
                  app.chatState().unreadCount(conversation.contact.userId));
      item.insert(
          QStringLiteral("latest"),
          messageJson(conversation.latest, conversation.contact.userId));
      result.append(item);
      if (result.size() >= limit)
        break;
    }
    return jsonResult({{QStringLiteral("conversations"), result},
                       {QStringLiteral("count"), result.size()}});
  }

  static MaiToolResult getMessages(RemoteIMApplication &app,
                                   const QJsonObject &arguments) {
    const QString peerId =
        arguments.value(QStringLiteral("peer_id")).toString().trimmed();
    if (peerId.isEmpty())
      return invalidInput(QStringLiteral("peer_id is required"));
    const QList<RemoteIMMessage> all = app.chatState().messagesWith(peerId);
    const int limit = boundedLimit(arguments);
    const int begin = std::max(0, all.size() - limit);
    QJsonArray messages;
    for (int i = begin; i < all.size(); ++i)
      messages.append(messageJson(all.at(i), peerId));
    return jsonResult({{QStringLiteral("peer_id"), peerId},
                       {QStringLiteral("messages"), messages},
                       {QStringLiteral("count"), messages.size()}});
  }

  static MaiToolResult searchMessages(RemoteIMApplication &app,
                                      const QJsonObject &arguments) {
    const QString query =
        arguments.value(QStringLiteral("query")).toString().trimmed();
    const QString onlyPeer =
        arguments.value(QStringLiteral("peer_id")).toString().trimmed();
    if (query.isEmpty())
      return invalidInput(QStringLiteral("query is required"));
    const int limit = boundedLimit(arguments);
    QJsonArray matches;
    for (const RemoteIMContact &contact : app.chatState().contacts()) {
      if (!onlyPeer.isEmpty() && onlyPeer != contact.userId)
        continue;
      app.chatState().forEachMessageWith(
          contact.userId, [&](const RemoteIMMessage &message) {
            if (matches.size() < limit &&
                MessageSearch::matches(message.text, query))
              matches.append(messageJson(message, contact.userId));
          });
      if (matches.size() >= limit)
        break;
    }
    return jsonResult({{QStringLiteral("query"), query},
                       {QStringLiteral("matches"), matches},
                       {QStringLiteral("count"), matches.size()}});
  }

  static MaiToolResult unreadSummary(RemoteIMApplication &app) {
    int total = 0;
    QJsonArray conversations;
    for (const RemoteIMContact &contact : app.chatState().contacts()) {
      const int unread = app.chatState().unreadCount(contact.userId);
      if (unread <= 0)
        continue;
      total += unread;
      conversations.append(
          QJsonObject{{QStringLiteral("peer_id"), contact.userId},
                      {QStringLiteral("display_name"), contact.displayName},
                      {QStringLiteral("unread"), unread}});
    }
    return jsonResult({{QStringLiteral("total_unread"), total},
                       {QStringLiteral("conversations"), conversations}});
  }

  static MaiToolResult sendText(RemoteIMApplication &app,
                                const QJsonObject &arguments) {
    const QString peerId =
        arguments.value(QStringLiteral("peer_id")).toString().trimmed();
    const QString text =
        arguments.value(QStringLiteral("text")).toString().trimmed();
    if (peerId.isEmpty() || text.isEmpty())
      return invalidInput(QStringLiteral("peer_id and text are required"));
    if (!hasContact(app.chatState(), peerId))
      return invalidInput(QStringLiteral("peer_id is not a MaiChat contact"));
    if (!app.sendTextTo(peerId, text))
      return MaiToolResult::failure(MaiErrorCode::Internal,
                                    "MaiChat did not queue the message");
    return jsonResult({{QStringLiteral("queued"), true},
                       {QStringLiteral("peer_id"), peerId}});
  }

  static MaiToolResult replyMessage(RemoteIMApplication &app,
                                    const QJsonObject &arguments) {
    const QString peerId =
        arguments.value(QStringLiteral("peer_id")).toString().trimmed();
    const QString messageId =
        arguments.value(QStringLiteral("message_id")).toString().trimmed();
    const QString text =
        arguments.value(QStringLiteral("text")).toString().trimmed();
    if (peerId.isEmpty() || messageId.isEmpty() || text.isEmpty())
      return invalidInput(
          QStringLiteral("peer_id, message_id, and text are required"));
    RemoteIMMessage quoted;
    bool found = false;
    app.chatState().forEachMessageWith(
        peerId, [&](const RemoteIMMessage &message) {
          if (!found && message.id == messageId) {
            quoted = message;
            found = true;
          }
        });
    if (!found)
      return invalidInput(
          QStringLiteral("message_id was not found for peer_id"));
    if (!app.sendTextTo(peerId, text, MessageQuote::quoteFor(quoted), true))
      return MaiToolResult::failure(MaiErrorCode::Internal,
                                    "MaiChat did not queue the reply");
    return jsonResult({{QStringLiteral("queued"), true},
                       {QStringLiteral("peer_id"), peerId},
                       {QStringLiteral("reply_to"), messageId}});
  }

  static MaiToolResult broadcastText(RemoteIMApplication &app,
                                     const QJsonObject &arguments) {
    const QJsonArray values = arguments.value(QStringLiteral("peer_ids")).toArray();
    const QString text =
        arguments.value(QStringLiteral("text")).toString().trimmed();
    if (values.isEmpty() || text.isEmpty())
      return invalidInput(QStringLiteral("peer_ids and text are required"));
    if (values.size() > 200)
      return invalidInput(QStringLiteral("peer_ids cannot contain more than 200 contacts"));

    QStringList recipients;
    QSet<QString> seen;
    for (const QJsonValue &value : values) {
      const QString peerId = value.toString().trimmed();
      if (peerId.isEmpty() || seen.contains(peerId))
        continue;
      if (!hasContact(app.chatState(), peerId))
        return invalidInput(
            QStringLiteral("peer_ids contains a non-contact: %1").arg(peerId));
      seen.insert(peerId);
      recipients.append(peerId);
    }
    if (recipients.isEmpty())
      return invalidInput(QStringLiteral("peer_ids must contain at least one contact"));

    const int queued = app.broadcastText(recipients, text);
    if (queued != recipients.size())
      return MaiToolResult::failure(MaiErrorCode::Internal,
                                    "MaiChat did not queue every broadcast message");
    QJsonArray peerIds;
    for (const QString &peerId : recipients)
      peerIds.append(peerId);
    return jsonResult({{QStringLiteral("queued"), true},
                       {QStringLiteral("recipient_count"), queued},
                       {QStringLiteral("peer_ids"), peerIds}});
  }

  HostToolKind mKind;
  std::string mName;
  std::string mDescription;
  std::string mSchema;
  QPointer<RemoteIMApplication> mApp;
  bool mApproval = false;
};

void addTool(MaiToolRegistry &registry, HostToolKind kind, const char *name,
             const char *description, const char *schema,
             RemoteIMApplication &app, bool approval = false) {
  registry.add(std::make_unique<MaiChatHostTool>(kind, name, description,
                                                 schema, app, approval));
}

} // namespace

// 桌面宿主工具按读写能力注册；有外部副作用的工具必须经过审批，名称与 Schema 供模型发现。
void registerMaiChatHostTools(MaiToolRegistry &registry,
                              RemoteIMApplication &app) {
  addTool(
      registry, HostToolKind::ListContacts, "maichat_list_contacts",
      "List MaiChat contacts. Use query to filter by user ID or display name.",
      R"({"type":"object","properties":{"query":{"type":"string"},"limit":{"type":"integer","minimum":1,"maximum":200})"
          R"(}})",
      app);
  addTool(registry, HostToolKind::RequestFriend, "maichat_request_friend",
          "Send a MaiChat friend request by phone or account ID. Requires user approval. "
          "The recipient must accept.",
          R"({"type":"object","properties":{"account":{"type":"string"}},"required":["account"]})",
          app, true);
  addTool(registry, HostToolKind::SetNickname, "maichat_set_nickname",
          "Set or clear the current MaiChat account nickname. Empty string clears it. "
          "Requires user approval.",
          R"({"type":"object","properties":{"nickname":{"type":"string","maxLength":32}})"
          R"(,"required":["nickname"]})",
          app, true);
  addTool(
      registry, HostToolKind::ListConversations, "maichat_list_conversations",
      "List direct MaiChat contact conversations ordered by latest message and unread count. "
      "Use maichat_list_groups for group conversations.",
      R"({"type":"object","properties":{"limit":{"type":"integer","minimum":1,"maximum":200}}})",
      app);
  addTool(
      registry, HostToolKind::GetMessages, "maichat_get_messages",
      "Read recent messages in a MaiChat conversation with a specific contact.",
      R"({"type":"object","properties":{"peer_id":{"type":"string"},"limit":{"type":"integer","minimum":1,)"
          R"("maximum":200}},"required":["peer_id"]})",
      app);
  addTool(
      registry, HostToolKind::SearchMessages, "maichat_search_messages",
      "Search MaiChat message text across all contacts or within one contact.",
      R"({"type":"object","properties":{"query":{"type":"string"},"peer_id":{"type":"string"},)"
          R"("limit":{"type":"integer","minimum":1,"maximum":200}},"required":["query"]})",
      app);
  addTool(registry, HostToolKind::GetUnreadSummary,
          "maichat_get_unread_summary",
          "Summarize unread MaiChat messages by contact.",
          R"({"type":"object","properties":{}})", app);
  addTool(
      registry, HostToolKind::SendText, "maichat_send_text",
      "Queue a text message to a MaiChat contact. This changes external state "
      "and requires user approval.",
      R"({"type":"object","properties":{"peer_id":{"type":"string"},"text":{"type":"string"}},"required":["peer_id",)"
          R"("text"]})",
      app, true);
  addTool(
      registry, HostToolKind::SendMedia, "maichat_send_media",
      "Send an Agent workspace image or video as a persistent MaiChat message "
      "bubble, or audio as a file card. Optional caption is kept with the media. "
      "This changes external state and requires user approval.",
      R"({"type":"object","properties":{"peer_id":{"type":"string"},"file_path":{"type":"string"},)"
          R"("type":{"type":"string","enum":["image","video","audio"]},"caption":{"type":"string"}},)"
          R"("required":["peer_id","file_path","type"],"additionalProperties":false})",
      app, true);
  addTool(
      registry, HostToolKind::ReplyMessage, "maichat_reply_message",
      "Queue a quoted text reply to a MaiChat message. This changes external "
      "state and requires user approval.",
      R"({"type":"object","properties":{"peer_id":{"type":"string"},"message_id":{"type":"string"},)"
          R"("text":{"type":"string"}},"required":["peer_id","message_id","text"]})",
      app, true);
  addTool(
      registry, HostToolKind::BroadcastText, "maichat_broadcast_text",
      "Queue the same text message to multiple MaiChat contacts. The complete "
      "recipient list and text require user approval for every call.",
      R"({"type":"object","properties":{"peer_ids":{"type":"array","items":{"type":"string"},"minItems":1,)"
          R"("maxItems":200},"text":{"type":"string"}},"required":["peer_ids","text"]})",
      app, true);
  addTool(registry, HostToolKind::ListGroups, "maichat_list_groups",
          "List joined MaiChat Work groups by name or group ID.",
          R"({"type":"object","properties":{"query":{"type":"string"},"limit":{"type":"integer","minimum":1,"maximum":200}},"additionalProperties":false})",
          app);
  addTool(registry, HostToolKind::CreateGroup, "maichat_create_group",
          "Create a MaiChat Work group with accepted friend IDs. Requires approval for each call.",
          R"({"type":"object","properties":{"name":{"type":"string","minLength":1},"member_ids":{"type":"array","items":{"type":"string"},"minItems":1,"maxItems":19,"uniqueItems":true}},"required":["name","member_ids"],"additionalProperties":false})",
          app, true);
  addTool(registry, HostToolKind::GetGroupMessages, "maichat_get_group_messages",
          "Read up to 20 recent text messages from a joined MaiChat group.",
          R"({"type":"object","properties":{"group_id":{"type":"string"},"limit":{"type":"integer","minimum":1,"maximum":20}},"required":["group_id"],"additionalProperties":false})",
          app);
  addTool(registry, HostToolKind::SendGroupText, "maichat_send_group_text",
          "Send a text message to a joined MaiChat group. Requires approval for each call.",
          R"({"type":"object","properties":{"group_id":{"type":"string"},"text":{"type":"string","minLength":1}},"required":["group_id","text"],"additionalProperties":false})",
          app, true);
}
