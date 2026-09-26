#include "app/MaiChatHostTools.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QPointer>
#include <QThread>

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
  ListConversations,
  GetMessages,
  SearchMessages,
  GetUnreadSummary,
  SendText,
  ReplyMessage,
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

  MaiToolResult execute(const std::string &argumentsJson,
                        const MaiToolContext &) override {
    QJsonObject arguments;
    QString parseError;
    if (!parseObject(argumentsJson, &arguments, &parseError))
      return invalidInput(parseError);
    return onApplicationThread(mApp, [&](RemoteIMApplication &app) {
      switch (mKind) {
      case HostToolKind::ListContacts:
        return listContacts(app, arguments);
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
      case HostToolKind::ReplyMessage:
        return replyMessage(app, arguments);
      }
      return MaiToolResult::failure(MaiErrorCode::Internal,
                                    "unknown MaiChat host tool");
    });
  }

private:
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

void registerMaiChatHostTools(MaiToolRegistry &registry,
                              RemoteIMApplication &app) {
  addTool(
      registry, HostToolKind::ListContacts, "maichat_list_contacts",
      "List MaiChat contacts. Use query to filter by user ID or display name.",
      R"({"type":"object","properties":{"query":{"type":"string"},"limit":{"type":"integer","minimum":1,"maximum":200}}})",
      app);
  addTool(
      registry, HostToolKind::ListConversations, "maichat_list_conversations",
      "List MaiChat conversations ordered by latest message, including unread "
      "counts.",
      R"({"type":"object","properties":{"limit":{"type":"integer","minimum":1,"maximum":200}}})",
      app);
  addTool(
      registry, HostToolKind::GetMessages, "maichat_get_messages",
      "Read recent messages in a MaiChat conversation with a specific contact.",
      R"({"type":"object","properties":{"peer_id":{"type":"string"},"limit":{"type":"integer","minimum":1,"maximum":200}},"required":["peer_id"]})",
      app);
  addTool(
      registry, HostToolKind::SearchMessages, "maichat_search_messages",
      "Search MaiChat message text across all contacts or within one contact.",
      R"({"type":"object","properties":{"query":{"type":"string"},"peer_id":{"type":"string"},"limit":{"type":"integer","minimum":1,"maximum":200}},"required":["query"]})",
      app);
  addTool(registry, HostToolKind::GetUnreadSummary,
          "maichat_get_unread_summary",
          "Summarize unread MaiChat messages by contact.",
          R"({"type":"object","properties":{}})", app);
  addTool(
      registry, HostToolKind::SendText, "maichat_send_text",
      "Queue a text message to a MaiChat contact. This changes external state "
      "and requires user approval.",
      R"({"type":"object","properties":{"peer_id":{"type":"string"},"text":{"type":"string"}},"required":["peer_id","text"]})",
      app, true);
  addTool(
      registry, HostToolKind::ReplyMessage, "maichat_reply_message",
      "Queue a quoted text reply to a MaiChat message. This changes external "
      "state and requires user approval.",
      R"({"type":"object","properties":{"peer_id":{"type":"string"},"message_id":{"type":"string"},"text":{"type":"string"}},"required":["peer_id","message_id","text"]})",
      app, true);
}
